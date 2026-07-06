/* ============================================================
 * cloud.c — modul integrare cloud pentru asistentul vocal
 *
 * Flux: wake word -> înregistrare PSRAM cu VAD -> POST WAV la server
 *       -> primire WAV răspuns -> redare prin audio_out
 *
 * Dependențe proiect:
 *   audio_out.h   — audio_out_write(samples, n_samples)
 *   s_afe_handle  — extern, definit în main.c
 *   s_afe_data    — extern, definit în main.c
 * ============================================================ */

#include "cloud.h"
#include "audio_out.h"
#include "web.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include <strings.h>
#include <string.h>
#include <stdint.h>

static const char *TAG = "cloud";
static char s_resp_transcript[256];
static char s_resp_reply[256];
/* ------------------------------------------------------------------ */
/*  Externe — definite în main.c                                      */
/* ------------------------------------------------------------------ */
extern esp_afe_sr_iface_t *s_afe_handle;
extern esp_afe_sr_data_t *s_afe_data;
extern volatile bool s_cloud_busy; /* oprește feed-ul în timpul HTTP/redare */

/* ------------------------------------------------------------------ */
/*  Stări LED/OLED (adaptat la ce folosești tu)                       */
/* ------------------------------------------------------------------ */
/* Comentează sau înlocuiește cu funcțiile tale reale */
static void ui_listening(void)
{
#ifdef CONFIG_HAS_OLED
    oled_show_status("Listening...");
#endif
#ifdef CONFIG_HAS_LED
    led_set_state(LED_BLUE); /* albastru = înregistrez */
#endif
    ESP_LOGI(TAG, "[UI] Listening");
}

static void ui_thinking(void)
{
#ifdef CONFIG_HAS_OLED
    oled_show_status("Thinking...");
#endif
#ifdef CONFIG_HAS_LED
    led_set_state(LED_YELLOW); /* galben = aștept server */
#endif
    ESP_LOGI(TAG, "[UI] Thinking");
}

static void ui_speaking(void)
{
#ifdef CONFIG_HAS_OLED
    oled_show_status("Speaking...");
#endif
#ifdef CONFIG_HAS_LED
    led_set_state(LED_GREEN); /* verde = redau răspuns */
#endif
    ESP_LOGI(TAG, "[UI] Speaking");
}

static void ui_idle(void)
{
#ifdef CONFIG_HAS_OLED
    oled_show_status("Say: Hi, ESP");
#endif
#ifdef CONFIG_HAS_LED
    led_set_state(LED_IDLE);
#endif
    ESP_LOGI(TAG, "[UI] Idle");
}

static void ui_error(void)
{
#ifdef CONFIG_HAS_OLED
    oled_show_status("Error :(");
#endif
#ifdef CONFIG_HAS_LED
    led_set_state(LED_RED);
#endif
    ESP_LOGI(TAG, "[UI] Error");
    vTaskDelay(pdMS_TO_TICKS(1500));
}

/* ------------------------------------------------------------------ */
/*  WAV header builder                                                 */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed))
{
    /* RIFF chunk */
    char riff_id[4];    /* "RIFF" */
    uint32_t riff_size; /* file_size - 8 */
    char wave_id[4];    /* "WAVE" */
    /* fmt  chunk */
    char fmt_id[4];           /* "fmt " */
    uint32_t fmt_size;        /* 16 */
    uint16_t audio_format;    /* 1 = PCM */
    uint16_t num_channels;    /* 1 = mono */
    uint32_t sample_rate;     /* 16000 */
    uint32_t byte_rate;       /* sample_rate * block_align */
    uint16_t block_align;     /* channels * bits/8 */
    uint16_t bits_per_sample; /* 16 */
    /* data chunk */
    char data_id[4];    /* "data" */
    uint32_t data_size; /* n_bytes de PCM */
} wav_header_t;

static void build_wav_header(wav_header_t *h, uint32_t pcm_bytes,
                             uint32_t sr, uint16_t ch, uint16_t bps)
{
    memcpy(h->riff_id, "RIFF", 4);
    h->riff_size = pcm_bytes + sizeof(wav_header_t) - 8;
    memcpy(h->wave_id, "WAVE", 4);
    memcpy(h->fmt_id, "fmt ", 4);
    h->fmt_size = 16;
    h->audio_format = 1;
    h->num_channels = ch;
    h->sample_rate = sr;
    h->bits_per_sample = bps;
    h->block_align = ch * (bps / 8);
    h->byte_rate = sr * h->block_align;
    memcpy(h->data_id, "data", 4);
    h->data_size = pcm_bytes;
}

/* ------------------------------------------------------------------ */
/*  HTTP response accumulator                                          */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint8_t *buf;
    size_t len;
    size_t capacity;
} http_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buf_t *rb = (http_buf_t *)evt->user_data;
    if (!rb)
        return ESP_OK;

    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        if (!evt->data_len)
            break;
        /* crește bufferul dacă e nevoie */
        if (rb->len + evt->data_len > rb->capacity)
        {
            size_t new_cap = rb->capacity + evt->data_len + 4096;
            uint8_t *tmp = heap_caps_realloc(rb->buf, new_cap,
                                             MALLOC_CAP_SPIRAM |
                                                 MALLOC_CAP_8BIT);
            if (!tmp)
            {
                ESP_LOGE(TAG, "realloc response buf failed (%u)", new_cap);
                return ESP_FAIL;
            }
            rb->buf = tmp;
            rb->capacity = new_cap;
        }
        memcpy(rb->buf + rb->len, evt->data, evt->data_len);
        rb->len += evt->data_len;
        break;
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(evt->header_key, "X-Transcript") == 0)
        {
            strncpy(s_resp_transcript, evt->header_value, sizeof(s_resp_transcript) - 1);
            s_resp_transcript[sizeof(s_resp_transcript) - 1] = '\0';
        }
        else if (strcasecmp(evt->header_key, "X-Reply") == 0)
        {
            strncpy(s_resp_reply, evt->header_value, sizeof(s_resp_reply) - 1);
            s_resp_reply[sizeof(s_resp_reply) - 1] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  cloud_record_and_query — funcția principală                       */
/* ------------------------------------------------------------------ */
esp_err_t cloud_record_and_query(void)
{
    /* ---------- 1. Alocare buffer înregistrare în PSRAM ---------- */
    const size_t max_samples = CLOUD_SAMPLE_RATE * CLOUD_MAX_SEC;
    const size_t max_bytes = max_samples * sizeof(int16_t);
    s_resp_transcript[0] = '\0';
    s_resp_reply[0] = '\0';
    int16_t *rec_buf = heap_caps_malloc(max_bytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rec_buf)
    {
        ESP_LOGE(TAG, "PSRAM malloc %u bytes failed", max_bytes);
        return ESP_FAIL;
    }
    size_t rec_samples = 0;

    /* ---------- 2. Înregistrare cu VAD ----------------------------- */
    ui_listening();
    ESP_LOGI(TAG, "Recording (max %ds, silence stop %dms)...",
             CLOUD_MAX_SEC, CLOUD_SILENCE_MS);

    /*
     * Numărul de frames AFE consecutive cu tăcere (VAD_SPEECH_OVER) înainte
     * de a considera că utilizatorul a terminat de vorbit.
     * Un frame AFE = 32ms (tipic pentru esp-sr 2.x).
     */
    const int silence_frames_threshold = CLOUD_SILENCE_MS / 32;
    int silence_frames = 0;
    bool got_speech = false;

    /* Dimensiunea unui frame AFE (în sample-uri) — constantă pe toată sesiunea */
    int frame_samples = s_afe_handle->get_fetch_chunksize(s_afe_data);

    while (rec_samples < max_samples)
    {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res || !res->data)
            continue;

        if (res->vad_state == VAD_SPEECH)
        {
            got_speech = true;
            silence_frames = 0;
        }
        else
        {
            /* Tăcere — dacă am mai primit și voce înainte, numărăm */
            if (got_speech)
                silence_frames++;
        }

        /* Copiăm frame-ul în buffer (indiferent de VAD, ca să nu tăiem începuturi) */
        size_t to_copy = frame_samples;
        if (rec_samples + to_copy > max_samples)
            to_copy = max_samples - rec_samples;
        memcpy(rec_buf + rec_samples, res->data, to_copy * sizeof(int16_t));
        rec_samples += to_copy;

        /* Stop dacă avem voce urmată de tăcere suficientă */
        if (got_speech && silence_frames >= silence_frames_threshold)
        {
            ESP_LOGI(TAG, "VAD: speech ended after %u frames silence", silence_frames);
            break;
        }
    }

    ESP_LOGI(TAG, "Recorded %u samples (%.2f s)", rec_samples,
             (float)rec_samples / CLOUD_SAMPLE_RATE);

    if (!got_speech || rec_samples < CLOUD_SAMPLE_RATE / 4)
    {
        /* Sub 250ms de voce → ignorăm */
        ESP_LOGW(TAG, "No speech detected, aborting");
        heap_caps_free(rec_buf);
        ui_idle();
        return ESP_OK;
    }

    /* Înregistrarea s-a terminat. De aici încolo (WAV + HTTP + redare)
     * nu mai citim din AFE, deci punem feed-ul în pauză ca să nu se
     * umple ringbuffer-ul cu spam. */
    s_cloud_busy = true;

    /* ---------- 3. Construim fișierul WAV în PSRAM ----------------- */
    const uint32_t pcm_bytes = rec_samples * sizeof(int16_t);
    const size_t wav_size = sizeof(wav_header_t) + pcm_bytes;

    uint8_t *wav_buf = heap_caps_malloc(wav_size,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav_buf)
    {
        ESP_LOGE(TAG, "PSRAM malloc WAV buf %u bytes failed", wav_size);
        heap_caps_free(rec_buf);
        ui_error();
        return ESP_FAIL;
    }

    wav_header_t hdr;
    build_wav_header(&hdr, pcm_bytes, CLOUD_SAMPLE_RATE, 1, 16);
    memcpy(wav_buf, &hdr, sizeof(wav_header_t));
    memcpy(wav_buf + sizeof(hdr), rec_buf, pcm_bytes);
    heap_caps_free(rec_buf); /* eliberăm PCM raw — nu mai e nevoie */

    /* ---------- 4. HTTP POST la server ----------------------------- */
    ui_thinking();

    http_buf_t resp = {.buf = NULL, .len = 0, .capacity = 0};
    /* pre-alocare inițială 64KB (răspunsul audio e de obicei 50-200KB) */
    resp.buf = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (resp.buf)
        resp.capacity = 65536;

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             CLOUD_SERVER_IP, CLOUD_SERVER_PORT, CLOUD_ENDPOINT);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CLOUD_HTTP_TIMEOUT,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "http_client_init failed");
        heap_caps_free(wav_buf);
        heap_caps_free(resp.buf);
        ui_error();
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_post_field(client, (const char *)wav_buf, (int)wav_size);

    ESP_LOGI(TAG, "POST %s (%u bytes WAV)...", url, wav_size);
    esp_err_t http_err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    heap_caps_free(wav_buf);

    if (http_err != ESP_OK || status != 200)
    {
        ESP_LOGE(TAG, "HTTP error: %s, status %d",
                 esp_err_to_name(http_err), status);
        heap_caps_free(resp.buf);
        ui_error();
        return ESP_OK; /* non-fatal — revenim la idle */
    }

    ESP_LOGI(TAG, "Received %u bytes from server (status %d)", resp.len, status);

    /* ---------- 5. Redare răspuns WAV ------------------------------ */
    if (resp.len <= sizeof(wav_header_t))
    {
        ESP_LOGW(TAG, "Response too small to be a WAV (%u bytes)", resp.len);
        heap_caps_free(resp.buf);
        ui_error();
        return ESP_OK;
    }

    ui_speaking();

    /*
     * Serverul returnează WAV 16 kHz mono 16-bit (exact ce acceptă audio_out).
     * Sărim header-ul (44 bytes) și trimitem PCM ca int16_t.
     */
    const int16_t *pcm = (const int16_t *)(resp.buf + sizeof(wav_header_t));
    size_t pcm_bytes_total = resp.len - sizeof(wav_header_t);
    size_t total_samples = pcm_bytes_total / sizeof(int16_t);

    /*
     * audio_out_write() primește (samples, n_samples). Trimitem în bucăți
     * de 2048 sample-uri ca să nu blocăm watchdog-ul.
     */
    const size_t CHUNK_SAMPLES = 2048;
    size_t sent = 0;
    while (sent < total_samples)
    {
        size_t n = total_samples - sent;
        if (n > CHUNK_SAMPLES)
            n = CHUNK_SAMPLES;
        audio_out_write(pcm + sent, n);
        sent += n;
    }

    heap_caps_free(resp.buf);

    /* ---------- 6. Revenire la idle -------------------------------- */
    ui_idle();
    ESP_LOGI(TAG, "cloud_record_and_query done");
    web_push_conversation(s_resp_transcript, s_resp_reply);
    return ESP_OK;
}

/* ================================================================== */
/*  cloud_query_text — întrebare scrisă din web (endpoint /text)      */
/* ================================================================== */
void cloud_query_text(const char *text)
{
    if (!text || !text[0])
        return;
    s_cloud_busy = true; // pune pe pauză feed-ul audio
    s_resp_transcript[0] = '\0';
    s_resp_reply[0] = '\0';

    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d/text",
             CLOUD_SERVER_IP, CLOUD_SERVER_PORT);

    // corp JSON {"text":"..."}
    char body[300];
    snprintf(body, sizeof(body), "{\"text\":\"%s\"}", text);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = CLOUD_HTTP_TIMEOUT,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, body, strlen(body));

    // NB: răspunsul audio este colectat în aceeași zonă tampon ca la /voice
    //     (vezi http_event_handler -> HTTP_EVENT_ON_DATA). După perform,
    //     redă cu aceeași secvență de audio_out_write folosită la /voice.
    esp_err_t err = esp_http_client_perform(cli);
    if (err == ESP_OK)
    {
        /* ... redă bufferul audio primit, identic cu cloud_record_and_query ... */
        web_push_conversation(NULL, s_resp_reply); // doar răspunsul
    }
    else
    {
        ESP_LOGE(TAG, "cloud_query_text: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(cli);
    s_cloud_busy = false;
}