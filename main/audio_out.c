/*
 * audio_out.c - I2S TX + sinteza sinus + acces streaming pentru TTS
 *
 * Rata 16 kHz - aliniata cu WAV-urile TTS. Tonurile suna identic
 * (frecventele notelor sunt absolute, independente de sample rate).
 *
 * Mutex pe scrierea I2S: tonurile si TTS-ul nu se suprapun.
 */
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#include "audio_out.h"

#define AUDIO_SR 16000 /* Hz - identic cu sample-urile TTS */
#define AUDIO_AMP 8000
#define ATTACK_MS 10
#define RELEASE_MS 30

#define M_PI_F 3.14159265358979323846f

static const char *TAG = "AUDIO";
static i2s_chan_handle_t s_tx = NULL;
static QueueHandle_t s_q = NULL;
static SemaphoreHandle_t s_write_mtx = NULL;

/* ------ Acces I2S protejat ------ */
void audio_out_write(const int16_t *samples, size_t n_samples)
{
    if (!s_tx || !samples || n_samples == 0)
        return;
    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    size_t written = 0;
    i2s_channel_write(s_tx, samples, n_samples * sizeof(int16_t),
                      &written, portMAX_DELAY);
    xSemaphoreGive(s_write_mtx);
}

/* ------ Sinteza sinus cu envelope ------ */
static int gen_sine(float freq_hz, int duration_ms, int16_t **out_buf)
{
    int n_samples = (AUDIO_SR * duration_ms) / 1000;
    int16_t *buf = (int16_t *)malloc(n_samples * sizeof(int16_t));
    if (!buf)
    {
        *out_buf = NULL;
        return 0;
    }

    int attack_samples = (AUDIO_SR * ATTACK_MS) / 1000;
    int release_samples = (AUDIO_SR * RELEASE_MS) / 1000;
    if (attack_samples > n_samples / 2)
        attack_samples = n_samples / 2;
    if (release_samples > n_samples / 2)
        release_samples = n_samples / 2;

    float phase_inc = 2.0f * M_PI_F * freq_hz / (float)AUDIO_SR;
    float phase = 0.0f;

    for (int i = 0; i < n_samples; i++)
    {
        float env = 1.0f;
        if (i < attack_samples)
            env = (float)i / (float)attack_samples;
        else if (i > n_samples - release_samples)
            env = (float)(n_samples - i) / (float)release_samples;
        buf[i] = (int16_t)(sinf(phase) * env * AUDIO_AMP);
        phase += phase_inc;
        if (phase >= 2.0f * M_PI_F)
            phase -= 2.0f * M_PI_F;
    }
    *out_buf = buf;
    return n_samples;
}

#define NOTE_A3 220.0f
#define NOTE_C4 261.63f
#define NOTE_E4 329.63f
#define NOTE_G4 392.0f
#define NOTE_C5 523.25f
#define NOTE_E5 659.25f

static void play_note(float freq, int duration_ms)
{
    int16_t *buf = NULL;
    int n = gen_sine(freq, duration_ms, &buf);
    if (n > 0)
    {
        audio_out_write(buf, n);
        free(buf);
    }
}

static void play_silence(int duration_ms)
{
    int n = (AUDIO_SR * duration_ms) / 1000;
    int16_t *buf = (int16_t *)calloc(n, sizeof(int16_t));
    if (buf)
    {
        audio_out_write(buf, n);
        free(buf);
    }
}

static void play_pattern(audio_sound_t s)
{
    switch (s)
    {
    case AUDIO_BOOT:
        play_note(NOTE_C4, 120);
        play_note(NOTE_E4, 120);
        play_note(NOTE_G4, 180);
        break;
    case AUDIO_WAKE:
        play_note(NOTE_E5, 150);
        break;
    case AUDIO_CMD_OK:
        play_note(NOTE_C5, 100);
        play_silence(30);
        play_note(NOTE_E5, 150);
        break;
    case AUDIO_CMD_FAIL:
        play_note(NOTE_A3, 350);
        break;
    }
}

static void audio_task(void *arg)
{
    audio_sound_t s;
    while (1)
    {
        if (xQueueReceive(s_q, &s, portMAX_DELAY) == pdTRUE)
        {
            play_pattern(s);
        }
    }
}

void audio_out_init(int gpio_bclk, int gpio_lrc, int gpio_din)
{
    s_write_mtx = xSemaphoreCreateMutex();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &s_tx, NULL) != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_new_channel TX esuat - audio out dezactivat");
        s_tx = NULL;
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = gpio_bclk,
            .ws = gpio_lrc,
            .dout = gpio_din,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };

    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(s_tx) != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s TX init/enable esuat");
        s_tx = NULL;
        return;
    }

    s_q = xQueueCreate(4, sizeof(audio_sound_t));
    xTaskCreatePinnedToCore(audio_task, "audio_out", 4096, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "Audio out: BCLK=GPIO%d LRC=GPIO%d DIN=GPIO%d @ %d Hz",
             gpio_bclk, gpio_lrc, gpio_din, AUDIO_SR);
}

void audio_play(audio_sound_t s)
{
    if (s_q)
        xQueueSend(s_q, &s, 0);
}