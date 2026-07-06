/*
 * ============================================================================
 *  ETAPA E3 — TTS: asistentul vorbeste (concatenare WAV de pe SPIFFS)
 * ============================================================================
 *  Schimbari fata de E1 (OLED):
 *    + dht11.h/c    - driver one-wire pentru senzor DHT11
 *    + audio_out.h/c - I2S TX cu sinteza sinus prin MAX98357A
 *    + Comenzile 4/5 (temperature/humidity) folosesc valori reale din DHT11
 *      (fallback la mock daca DHT11 nu raspunde inca)
 *    + La fiecare eveniment audio: si buzzer (existent) si audio prin difuzor
 *      (paralel - pentru documentatie demonstrez ambele solutii)
 *
 *  Pinout NOU:
 *    - DHT11 DATA: GPIO15
 *    - MAX98357A BCLK: GPIO38
 *    - MAX98357A LRC : GPIO39
 *    - MAX98357A DIN : GPIO40
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

#include "led_strip.h"

#include "commands.h"
#include "state.h"
#include "web.h"
#include "buzzer.h"
#include "oled_ui.h"
#include "dht11.h" /* <-- NOU */
#include "audio_out.h"
#include "tts.h" /* <-- NOU E3 */
#include "cloud.h"

#define I2S_BCLK_GPIO GPIO_NUM_4
#define I2S_WS_GPIO GPIO_NUM_5
#define I2S_DIN_GPIO GPIO_NUM_6
#define LED_LAMP_GPIO GPIO_NUM_17
#define LED_RGB_GPIO GPIO_NUM_48
#define I2C_SDA_GPIO GPIO_NUM_8
#define I2C_SCL_GPIO GPIO_NUM_9
#define DHT11_GPIO GPIO_NUM_15      /* <-- NOU */
#define AUDIO_BCLK_GPIO GPIO_NUM_38 /* <-- NOU */
#define AUDIO_LRC_GPIO GPIO_NUM_39  /* <-- NOU */
#define AUDIO_DIN_GPIO GPIO_NUM_40  /* <-- NOU */

#define SAMPLE_RATE 16000
#define CMD_WINDOW_MS 3000

typedef struct
{
    uint8_t r, g, b;
} rgb_t;
static const rgb_t COLOR_OFF = {0, 0, 0};
static const rgb_t COLOR_BLUE = {0, 0, 80};
static const rgb_t COLOR_GREEN = {0, 80, 0};
static const rgb_t COLOR_RED = {80, 0, 0};

static const char *TAG = "VA";

static i2s_chan_handle_t s_rx_handle = NULL;
esp_afe_sr_iface_t *s_afe_handle = NULL;
esp_afe_sr_data_t *s_afe_data = NULL;
volatile bool s_cloud_busy = false;
volatile bool s_mode_online = true;
static esp_mn_iface_t *s_mn_handle = NULL;
static model_iface_data_t *s_mn_data = NULL;
static led_strip_handle_t s_rgb = NULL;
static float g_last_temp = 0.0f;
static float g_last_hum = 0.0f;

typedef enum
{
    STATE_IDLE,
    STATE_LISTENING_CMD
} va_sm_t;
static volatile va_sm_t s_sm = STATE_IDLE;
static volatile int64_t s_cmd_end = 0;
static volatile bool s_lamp_on = false;
static inline void lamp_set(bool on);
static void blink_task(void *arg); /* definit mai jos — declarat aici ca să fie vizibil în web_on_light */

static const char *k_jokes[] = {
    "Why did the developer go broke? Because he used up all his cache.",
    "Why do programmers prefer dark mode? Because light attracts bugs.",
    "What's a programmer's favorite hangout? The Foo Bar.",
    "Why did the function return early? It had commitment issues.",
    "I would tell you a UDP joke, but you might not get it.",
};
#define JOKES_COUNT (sizeof(k_jokes) / sizeof(k_jokes[0]))

static void push_state(void)
{
    va_state_t snap;
    state_snapshot(&snap);

    /* Ultima comandă = ultima intrare din istoricul circular */
    const char *last = "";
    if (snap.history_count > 0)
    {
        int idx = (snap.history_head - 1 + MAX_HISTORY) % MAX_HISTORY;
        last = snap.history[idx].phrase;
    }

    web_broadcast_state(s_mode_online ? "online" : "offline",
                        (int)snap.wake_count, /* nr. de wake-uri */
                        last,                 /* ultima comandă/text */
                        g_last_temp, g_last_hum);
}

void web_on_light(const char *val)
{
    if (strcmp(val, "on") == 0)
        lamp_set(true);
    else if (strcmp(val, "off") == 0)
        lamp_set(false);
    else if (strcmp(val, "blink") == 0)
        xTaskCreatePinnedToCore(blink_task, "blink", 2048, NULL, 4, NULL, 0);
    push_state();
}
void web_on_mode(const char *val)
{
    s_mode_online = (strcmp(val, "online") == 0);
    ESP_LOGI(TAG, "mod schimbat din web: %s", val);
    push_state();
}
void web_on_ask(const char *text)
{
    cloud_query_text(text); // trimite întrebarea scrisă la server
}

/* Helper: feedback audio dublu (buzzer simplu + speaker elegant) */
static inline void play_sound_dual(buzzer_sound_t b, audio_sound_t a)
{
    buzzer_play(b);
    audio_play(a);
}

static inline void notify_all_displays(void)
{
    push_state();
    oled_ui_request_refresh();
}

/* ----- Task DHT11 -> state ----- */
/* DHT11 task din dht11.c ruleaza fiecare 5s. Aici copiem in state pentru web/OLED. */
static void dht11_sync_task(void *arg)
{
    while (1)
    {
        if (dht11_has_valid_reading())
        {
            float t = dht11_get_temperature();
            float h = dht11_get_humidity();
            state_set_sensors(t, h);
            notify_all_displays();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ----- Hardware init ----- */
static void init_i2s_microphone(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = I2S_BCLK_GPIO, .ws = I2S_WS_GPIO, .dout = I2S_GPIO_UNUSED, .din = I2S_DIN_GPIO, .invert_flags = {0}},
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));
    ESP_LOGI(TAG, "I2S mic OK");
}

static void init_led_lamp(void)
{
    gpio_config_t io = {.pin_bit_mask = (1ULL << LED_LAMP_GPIO),
                        .mode = GPIO_MODE_OUTPUT,
                        .pull_up_en = GPIO_PULLUP_DISABLE,
                        .pull_down_en = GPIO_PULLDOWN_DISABLE,
                        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io);
    gpio_set_level(LED_LAMP_GPIO, 0);
}

static inline void lamp_set(bool on)
{
    s_lamp_on = on;
    gpio_set_level(LED_LAMP_GPIO, on ? 1 : 0);
    state_set_lamp(on);
    notify_all_displays();
}

static void init_led_rgb(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_RGB_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {.invert_out = false},
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {.with_dma = false},
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_rgb));
    led_strip_clear(s_rgb);
}

static void led_rgb_set(rgb_t c)
{
    led_strip_set_pixel(s_rgb, 0, c.r, c.g, c.b);
    led_strip_refresh(s_rgb);
}

static const voice_command_t *find_command(int id)
{
    for (size_t i = 0; i < COMMANDS_COUNT; i++)
        if (k_commands[i].id == id)
            return &k_commands[i];
    return NULL;
}

static void blink_task(void *arg)
{
    bool prev = s_lamp_on;
    for (int i = 0; i < 5; i++)
    {
        gpio_set_level(LED_LAMP_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(LED_LAMP_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    gpio_set_level(LED_LAMP_GPIO, prev ? 1 : 0);
    vTaskDelete(NULL);
}

/* ----- Dispatcher comenzi - acum cu DHT11 real ----- */
static void execute_command(int cmd_id)
{
    const voice_command_t *cmd = find_command(cmd_id);
    if (!cmd)
    {
        ESP_LOGW(TAG, "ID necunoscut: %d", cmd_id);
        return;
    }
    ESP_LOGI(TAG, ">>> Execut comanda #%d: %s", cmd_id, cmd->phrase);

    switch (cmd_id)
    {
    case 1:
        lamp_set(true);
        ESP_LOGI(TAG, "   Lampa APRINSA");
        tts_say_light_on();
        break;
    case 2:
        lamp_set(false);
        ESP_LOGI(TAG, "   Lampa STINSA");
        tts_say_light_off();
        break;
    case 3:
        ESP_LOGI(TAG, "   Lampa CLIPESTE");
        tts_say_light_blink();
        xTaskCreatePinnedToCore(blink_task, "blink", 2048, NULL, 4, NULL, 0);
        break;
    case 4:
    {
        /* Temperatura - din DHT11 real daca disponibil, altfel mock */
        float t;
        if (dht11_has_valid_reading())
        {
            t = dht11_get_temperature();
            ESP_LOGI(TAG, "   Temperatura: %.1f °C (DHT11)", t);
        }
        else
        {
            t = 21.5f + ((esp_random() % 4000) / 1000.0f);
            ESP_LOGI(TAG, "   Temperatura: %.1f °C (mock - DHT11 nu raspunde)", t);
        }
        state_set_sensors(t, dht11_has_valid_reading() ? dht11_get_humidity() : 50.0f);
        tts_say_temperature((int)(t + 0.5f));
        notify_all_displays();
        break;
    }
    case 5:
    {
        float h;
        if (dht11_has_valid_reading())
        {
            h = dht11_get_humidity();
            ESP_LOGI(TAG, "   Umiditate: %.1f %% (DHT11)", h);
        }
        else
        {
            h = 45.0f + ((esp_random() % 20000) / 1000.0f);
            ESP_LOGI(TAG, "   Umiditate: %.1f %% (mock - DHT11 nu raspunde)", h);
        }
        state_set_sensors(dht11_has_valid_reading() ? dht11_get_temperature() : 22.0f, h);
        tts_say_humidity((int)(h + 0.5f));
        notify_all_displays();
        break;
    }
    case 6:
    {
        int idx = esp_random() % JOKES_COUNT;
        ESP_LOGI(TAG, "   Joke: %s", k_jokes[idx]);
        tts_say_joke(idx + 1);
        break;
    }
    case 7:
        ESP_LOGI(TAG, "   Hello! Sunt asistentul tau vocal.");
        tts_say_hello();
        break;
    }
}

/* Păstrat pentru o eventuală declanșare a comenzilor vocale direct din browser
 * (momentan UI-ul folosește web_on_light/web_on_mode/web_on_ask). */
static void on_web_action(int cmd_id) __attribute__((unused));
static void on_web_action(int cmd_id)
{
    ESP_LOGI(TAG, "WEB action: cmd=%d", cmd_id);
    execute_command(cmd_id);
    const voice_command_t *cmd = find_command(cmd_id);
    if (cmd)
    {
        char tag[40];
        snprintf(tag, sizeof(tag), "[web] %s", cmd->phrase);
        state_add_history(cmd_id, tag, 1.0f);
        notify_all_displays();
    }
}

static void register_commands(void)
{
    esp_mn_commands_alloc(s_mn_handle, s_mn_data);
    esp_mn_commands_clear();
    for (size_t i = 0; i < COMMANDS_COUNT; i++)
    {
        esp_mn_commands_add(k_commands[i].id, (char *)k_commands[i].phrase);
        ESP_LOGI(TAG, "  CMD %d: \"%s\"", k_commands[i].id, k_commands[i].phrase);
    }
    esp_mn_commands_update();
}

static void audio_feed_task(void *arg)
{
    int n_samples = s_afe_handle->get_feed_chunksize(s_afe_data) * s_afe_handle->get_feed_channel_num(s_afe_data);
    int32_t *i2s_buf = heap_caps_malloc(n_samples * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    int16_t *afe_buf = heap_caps_malloc(n_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    while (1)
    {
        /* Cât timp cloud înregistrează/trimite/redă, nu mai băgăm în AFE */
        if (s_cloud_busy)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t bytes_read = 0;
        if (i2s_channel_read(s_rx_handle, i2s_buf, n_samples * sizeof(int32_t),
                             &bytes_read, portMAX_DELAY) != ESP_OK)
            continue;
        int n = bytes_read / sizeof(int32_t);
        for (int i = 0; i < n; i++)
            afe_buf[i] = (int16_t)(i2s_buf[i] >> 14);
        s_afe_handle->feed(s_afe_data, afe_buf);
    }
}

static void enter_idle(int delay_ms)
{
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    led_rgb_set(COLOR_OFF);
    s_sm = STATE_IDLE;
    state_set_listening_cmd(false);
    state_set_speech_detected(false);
    notify_all_displays();
    ESP_LOGI(TAG, "[IDLE] (lampa: %s, DHT: %s)",
             s_lamp_on ? "ON" : "OFF",
             dht11_has_valid_reading() ? "OK" : "wait");
}

static void detect_task(void *arg)
{
    ESP_LOGI(TAG, "[IDLE] Astept wake word 'Hi, ESP'...");
    bool prev_vad = false;

    while (1)
    {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        bool vad_now = (res->vad_state == VAD_SPEECH);
        if (vad_now != prev_vad)
        {
            state_set_speech_detected(vad_now);
            notify_all_displays();
            prev_vad = vad_now;
        }

        if (res->wakeup_state == WAKENET_DETECTED)
        {
            state_inc_wake_count();
            push_state();
            if (s_mode_online)
            {
                led_rgb_set(COLOR_BLUE);
                s_afe_handle->disable_wakenet(s_afe_data);
                cloud_record_and_query();
                s_cloud_busy = false;
                s_afe_handle->enable_wakenet(s_afe_data);
            }
            else
            {
                led_rgb_set(COLOR_BLUE);
                play_sound_dual(BUZZER_WAKE, AUDIO_WAKE);
                notify_all_displays();

                /* NU setăm s_cloud_busy aici — cloud.c îl gestionează intern:
                 * false în timpul înregistrării (ca feed-ul să meargă),
                 * true în timpul HTTP+redare. Noi doar îl forțăm false la final
                 * ca plasă de siguranță (în caz de return pe eroare). */
                s_afe_handle->disable_wakenet(s_afe_data);

                cloud_record_and_query();

                s_cloud_busy = false; /* plasă de siguranță */
                s_afe_handle->enable_wakenet(s_afe_data);

                state_set_listening_cmd(false);
                notify_all_displays();
                continue;
            }

            if (s_sm == STATE_LISTENING_CMD)
            {
                if (esp_timer_get_time() / 1000 > s_cmd_end)
                {
                    ESP_LOGW(TAG, "Timeout");
                    state_inc_commands_timeout();
                    led_rgb_set(COLOR_RED);
                    buzzer_play(BUZZER_CMD_FAIL);
                    tts_say_not_understood();
                    enter_idle(500);
                    continue;
                }

                esp_mn_state_t mn = s_mn_handle->detect(s_mn_data, res->data);
                if (mn == ESP_MN_STATE_DETECTED)
                {
                    esp_mn_results_t *r = s_mn_handle->get_results(s_mn_data);
                    if (r && r->num > 0)
                    {
                        int cmd_id = r->command_id[0];
                        float prob = r->prob[0];
                        const voice_command_t *cmd = find_command(cmd_id);
                        if (cmd)
                        {
                            ESP_LOGI(TAG, ">>> CMD #%d: \"%s\" (prob=%.2f)",
                                     cmd_id, cmd->phrase, prob);
                            led_rgb_set(COLOR_GREEN);
                            buzzer_play(BUZZER_CMD_OK); /* vorbirea vine din execute_command */
                            state_inc_commands_recognized();
                            state_add_history(cmd_id, cmd->phrase, prob);
                            execute_command(cmd_id);
                        }
                        else
                        {
                            led_rgb_set(COLOR_RED);
                            buzzer_play(BUZZER_CMD_FAIL);
                            tts_say_not_understood();
                        }
                        enter_idle(500);
                    }
                }
                else if (mn == ESP_MN_STATE_TIMEOUT)
                {
                    state_inc_commands_timeout();
                    led_rgb_set(COLOR_RED);
                    buzzer_play(BUZZER_CMD_FAIL);
                    tts_say_not_understood();
                    enter_idle(500);
                }
            }
        }
    }
}
void app_main(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  E3 - Voice Assistant cu TTS");
    ESP_LOGI(TAG, "  Vorbeste prin difuzor (concatenative TTS)");
    ESP_LOGI(TAG, "================================================");

    state_init();

    /* Hardware basic */
    init_led_lamp();
    init_led_rgb();
    init_i2s_microphone();
    buzzer_init();
    oled_ui_init(I2C_SDA_GPIO, I2C_SCL_GPIO);

    /* NOU: DHT11 + audio */
    dht11_init(DHT11_GPIO);
    audio_out_init(AUDIO_BCLK_GPIO, AUDIO_LRC_GPIO, AUDIO_DIN_GPIO);
    tts_init(); /* <-- NOU E3 */

    /* Animatie boot LED RGB */
    led_rgb_set(COLOR_RED);
    vTaskDelay(pdMS_TO_TICKS(150));
    led_rgb_set(COLOR_GREEN);
    vTaskDelay(pdMS_TO_TICKS(150));
    led_rgb_set(COLOR_BLUE);
    vTaskDelay(pdMS_TO_TICKS(150));
    led_rgb_set(COLOR_OFF);

    /* WiFi + server */
    web_start();

    /* Modele */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models || models->num <= 0)
    {
        ESP_LOGE(TAG, "Nu pot incarca modele!");
        return;
    }

    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = false;
    afe_config->se_init = true;
    afe_config->vad_init = true;
    afe_config->wakenet_init = true;
    s_afe_handle = esp_afe_handle_from_config(afe_config);
    s_afe_data = s_afe_handle->create_from_config(afe_config);

    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    s_mn_handle = esp_mn_handle_from_name(mn_name);
    s_mn_data = s_mn_handle->create(mn_name, CMD_WINDOW_MS);
    register_commands();

    ESP_LOGI(TAG, "Heap: intern=%u KB, SPIRAM=%u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    xTaskCreatePinnedToCore(audio_feed_task, "feed", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(detect_task, "detect", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(dht11_sync_task, "dht_sync", 6144, NULL, 2, NULL, 0);
    /* Boot complete: tonuri + voce */
    buzzer_play(BUZZER_BOOT);
    audio_play(AUDIO_BOOT);
    tts_say_ready(); /* "Voice assistant ready" */
    notify_all_displays();

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Gata - Asistent vocal complet operational");
    ESP_LOGI(TAG, "================================================");
}