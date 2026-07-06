/*
 * tts.c - concatenative TTS player
 *
 * Functionare:
 *   1. tts_init() monteaza SPIFFS (partitia "storage") la /spiffs
 *   2. Functiile tts_say_*() construiesc o lista de fisiere si o pun in coada
 *   3. tts_task citeste fisierele pe rand, sare header-ul WAV si streameaza
 *      PCM-ul direct la audio_out (I2S TX -> MAX98357A)
 *
 * Parsarea WAV: cautam chunk-ul "data" in structura RIFF (header-ele
 * generate de Windows TTS pot contine chunk-uri extra inainte de data).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "tts.h"
#include "audio_out.h"

#define TTS_MAX_PARTS 8  /* max fisiere intr-o fraza */
#define TTS_NAME_LEN 24  /* nume scurt fara cale/extensie */
#define CHUNK_BYTES 4096 /* citire/redare in bucati de 4KB */

static const char *TAG = "TTS";
static QueueHandle_t s_q = NULL;
static bool s_mounted = false;

typedef struct
{
    int count;
    char names[TTS_MAX_PARTS][TTS_NAME_LEN];
} tts_phrase_t;

/* ----------------------------------------------------------------------------
 * WAV: gaseste offset-ul si dimensiunea chunk-ului "data"
 * -------------------------------------------------------------------------- */
static bool wav_find_data(FILE *f, long *data_offset, uint32_t *data_size)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12)
        return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return false;

    /* Iteram chunk-urile */
    while (1)
    {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8)
            return false;
        uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                      ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(ch, "data", 4) == 0)
        {
            *data_offset = ftell(f);
            *data_size = sz;
            return true;
        }
        /* skip chunk (padded la 2) */
        if (fseek(f, (long)(sz + (sz & 1)), SEEK_CUR) != 0)
            return false;
    }
}

/* Reda un singur fisier WAV de pe SPIFFS */
static void play_wav(const char *short_name)
{
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/%s.wav", short_name);

    FILE *f = fopen(path, "rb");
    if (!f)
    {
        ESP_LOGW(TAG, "Lipsa fisier: %s", path);
        return;
    }

    long offset = 0;
    uint32_t size = 0;
    if (!wav_find_data(f, &offset, &size))
    {
        ESP_LOGW(TAG, "WAV invalid: %s", path);
        fclose(f);
        return;
    }

    static int16_t buf[CHUNK_BYTES / 2];
    uint32_t remaining = size;
    while (remaining > 0)
    {
        size_t to_read = remaining > CHUNK_BYTES ? CHUNK_BYTES : remaining;
        size_t got = fread(buf, 1, to_read, f);
        if (got == 0)
            break;
        audio_out_write(buf, got / 2); /* nr de sample-uri int16 */
        remaining -= got;
    }
    fclose(f);
}

static void tts_task(void *arg)
{
    tts_phrase_t ph;
    while (1)
    {
        if (xQueueReceive(s_q, &ph, portMAX_DELAY) == pdTRUE)
        {
            for (int i = 0; i < ph.count; i++)
            {
                play_wav(ph.names[i]);
            }
        }
    }
}

/* ----------------------------------------------------------------------------
 * Construire fraze
 * -------------------------------------------------------------------------- */
static void phrase_add(tts_phrase_t *ph, const char *name)
{
    if (ph->count >= TTS_MAX_PARTS)
        return;
    strncpy(ph->names[ph->count], name, TTS_NAME_LEN - 1);
    ph->names[ph->count][TTS_NAME_LEN - 1] = '\0';
    ph->count++;
}

/* Adauga numarul 0-99 ca unul sau doua sample-uri */
static void phrase_add_number(tts_phrase_t *ph, int n)
{
    char buf[TTS_NAME_LEN];
    if (n < 0)
        n = 0;
    if (n > 99)
        n = 99;

    if (n < 20)
    {
        snprintf(buf, sizeof(buf), "num_%d", n);
        phrase_add(ph, buf);
    }
    else
    {
        int tens = (n / 10) * 10;
        int ones = n % 10;
        snprintf(buf, sizeof(buf), "num_%d", tens);
        phrase_add(ph, buf);
        if (ones > 0)
        {
            snprintf(buf, sizeof(buf), "num_%d", ones);
            phrase_add(ph, buf);
        }
    }
}

static void enqueue(tts_phrase_t *ph)
{
    if (!s_mounted || !s_q || ph->count == 0)
        return;
    xQueueSend(s_q, ph, 0); /* drop daca plina */
}

/* ----------------------------------------------------------------------------
 * API public
 * -------------------------------------------------------------------------- */
bool tts_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "SPIFFS mount esuat (%s) - TTS dezactivat",
                 esp_err_to_name(ret));
        return false;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS montat: %u KB folositi / %u KB total",
             (unsigned)(used / 1024), (unsigned)(total / 1024));

    s_q = xQueueCreate(3, sizeof(tts_phrase_t));
    xTaskCreatePinnedToCore(tts_task, "tts", 4096, NULL, 4, NULL, 0);
    s_mounted = true;
    return true;
}

void tts_say_ready(void)
{
    tts_phrase_t ph = {0};
    phrase_add(&ph, "ready");
    enqueue(&ph);
}

void tts_say_hello(void)
{
    tts_phrase_t ph = {0};
    phrase_add(&ph, "hello");
    enqueue(&ph);
}

void tts_say_light_on(void)
{
    tts_phrase_t ph = {0};
    phrase_add(&ph, "light_on");
    enqueue(&ph);
}

void tts_say_light_off(void)
{
    tts_phrase_t ph = {0};
    phrase_add(&ph, "light_off");
    enqueue(&ph);
}

void tts_say_light_blink(void)
{
    tts_phrase_t ph = {0};
    phrase_add(&ph, "light_blink");
    enqueue(&ph);
}

void tts_say_not_understood(void)
{
    tts_phrase_t ph = {0};
    phrase_add(&ph, "not_understood");
    enqueue(&ph);
}

void tts_say_joke(int idx)
{
    if (idx < 1)
        idx = 1;
    if (idx > 5)
        idx = 5;
    char buf[TTS_NAME_LEN];
    snprintf(buf, sizeof(buf), "joke_%d", idx);
    tts_phrase_t ph = {0};
    phrase_add(&ph, buf);
    enqueue(&ph);
}

void tts_say_temperature(int deg)
{
    tts_phrase_t ph = {0};
    phrase_add(&ph, "the_temperature_is");
    phrase_add_number(&ph, deg);
    phrase_add(&ph, "degrees");
    enqueue(&ph);
}

void tts_say_humidity(int pct)
{
    tts_phrase_t ph = {0};
    phrase_add(&ph, "the_humidity_is");
    phrase_add_number(&ph, pct);
    phrase_add(&ph, "percent");
    enqueue(&ph);
}