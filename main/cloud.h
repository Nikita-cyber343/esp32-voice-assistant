#pragma once
#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * cloud.h - modul integrare cloud pentru asistentul vocal
     *
     * Flux:
     *   cloud_record_and_query()
     *     1. înregistrează audio din AFE în PSRAM (cu VAD, max 6s)
     *     2. construiește fișier WAV în memorie
     *     3. POST la http://<SERVER_IP>:<PORT>/voice
     *     4. primește WAV răspuns → îl redă prin audio_out
     *
     * Apelat din detect_task() după detecție wake word.
     */

#include "esp_err.h"

/* ------------------------------------------------------------------ */
/*  Configurare — ajustezi IP-ul serverului tău aici                  */
/* ------------------------------------------------------------------ */
#define CLOUD_SERVER_IP "192.168.100.151" /* <-- IP-ul laptopului tău */
#define CLOUD_SERVER_PORT 8000
#define CLOUD_ENDPOINT "/voice"

/* Parametri înregistrare */
#define CLOUD_SAMPLE_RATE 16000  /* Hz  — must match AFE */
#define CLOUD_MAX_SEC 6          /* secunde înregistrare max */
#define CLOUD_SILENCE_MS 1200    /* ms tăcere → stop înregistrare */
#define CLOUD_HTTP_TIMEOUT 60000 /* ms timeout HTTP total */

    /* ------------------------------------------------------------------ */
    /*  API public                                                         */
    /* ------------------------------------------------------------------ */

    /**
     * @brief  Înregistrează vocea, trimite la server, redă răspunsul.
     *
     * Funcția blochează task-ul apelant pe durata întregului ciclu
     * (înregistrare → HTTP → redare). OLED/LED-urile sunt actualizate intern.
     *
     * @return ESP_OK dacă ciclul s-a finalizat (inclusiv erori non-fatale
     *         gestionate intern), ESP_FAIL dacă memoria nu a putut fi alocată.
     */
    esp_err_t cloud_record_and_query(void);
    void cloud_query_text(const char *text); // <-- NOU (întrebare scrisă din web)
#ifdef __cplusplus
}
#endif