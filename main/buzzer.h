/*
 * buzzer.h - buzzer activ pe GPIO16
 *
 * Buzzer-ul are oscilator intern, deci doar HIGH/LOW. Diferentierea
 * sunetelor se face prin durata si pattern (numarul de pulse-uri).
 */
#ifndef VA_BUZZER_H
#define VA_BUZZER_H

typedef enum
{
    BUZZER_WAKE = 1,     /* 1 ding scurt - 80ms */
    BUZZER_CMD_OK = 2,   /* 2 ding-uri rapide */
    BUZZER_CMD_FAIL = 3, /* 1 buzz lung - 200ms */
    BUZZER_BOOT = 4,     /* 3 ding-uri rapide la pornire */
} buzzer_sound_t;

/* Initializeaza GPIO + task de redare (non-blocking) */
void buzzer_init(void);

/* Cere redarea unui sunet. Returneaza imediat. */
void buzzer_play(buzzer_sound_t s);

#endif