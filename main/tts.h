/*
 * tts.h - sinteza vocala prin concatenare de sample-uri WAV de pe SPIFFS
 *
 * Sample-urile (generate cu generate_tts.ps1, 16 kHz/mono/16-bit) sunt
 * stocate pe partitia "storage" montata la /spiffs.
 *
 * API non-blocking: functiile pun fraza in coada si revin imediat.
 * Task-ul TTS reda fisierele secvential prin audio_out.
 */
#ifndef VA_TTS_H
#define VA_TTS_H

#include <stdbool.h>

/* Monteaza SPIFFS + porneste task-ul TTS.
 * Returneaza false daca partitia nu poate fi montata (sistemul continua). */
bool tts_init(void);

/* Fraze fixe */
void tts_say_ready(void);          /* "Voice assistant ready" */
void tts_say_hello(void);          /* "Hello! I am your voice assistant" */
void tts_say_light_on(void);       /* "The light is now on" */
void tts_say_light_off(void);      /* "The light is now off" */
void tts_say_light_blink(void);    /* "Blinking the light" */
void tts_say_not_understood(void); /* "Sorry, I did not understand" */
void tts_say_joke(int idx);        /* joke_1 .. joke_5 */

/* Fraze compuse cu numere (0-99) */
void tts_say_temperature(int deg); /* "The temperature is twenty eight degrees" */
void tts_say_humidity(int pct);    /* "The humidity is thirty eight percent" */

#endif