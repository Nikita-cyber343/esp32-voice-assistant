/*
 * audio_out.h - audio output prin I2S TX -> MAX98357A -> difuzor 40mm
 *
 * 1. Tonuri muzicale (sinus) pentru feedback rapid
 * 2. audio_out_write() - acces direct pentru streaming PCM (folosit de TTS)
 *
 * Rata: 16 kHz (aliniata cu sample-urile TTS de pe SPIFFS)
 */
#ifndef VA_AUDIO_OUT_H
#define VA_AUDIO_OUT_H

#include <stdint.h>
#include <stddef.h>

typedef enum
{
    AUDIO_BOOT = 1,
    AUDIO_WAKE = 2,
    AUDIO_CMD_OK = 3,
    AUDIO_CMD_FAIL = 4,
} audio_sound_t;

/* Init driver I2S TX + task de redare tonuri */
void audio_out_init(int gpio_bclk, int gpio_lrc, int gpio_din);

/* Cere redarea unui ton (non-blocking) */
void audio_play(audio_sound_t s);

/* Scrie PCM 16-bit mono 16kHz direct la I2S (blocking pana pleaca tot).
 * Thread-safe: mutex intern previne suprapunerea cu tonurile. */
void audio_out_write(const int16_t *samples, size_t n_samples);

#endif