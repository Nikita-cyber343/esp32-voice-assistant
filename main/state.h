/*
 * state.h — stare globala partajata intre voice + web
 *
 * Toate accesele se fac prin functiile state_get_snapshot() / state_set_*
 * care iau intern un mutex FreeRTOS. Garanteaza consistenta intre task-urile
 * de voice si task-ul HTTP server.
 */

#ifndef VA_STATE_H
#define VA_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_HISTORY 10

typedef struct
{
    int cmd_id;           /* 0 daca slot gol */
    char phrase[40];      /* fraza recunoscuta */
    float prob;           /* probabilitate */
    int64_t timestamp_ms; /* esp_timer_get_time()/1000 la momentul detectiei */
} cmd_history_entry_t;

typedef struct
{
    /* Hardware state */
    bool lamp_on;

    /* Sensors (mock pana la B3) */
    float temperature_c;
    float humidity_pct;

    /* Stats */
    uint32_t wake_count;
    uint32_t commands_recognized;
    uint32_t commands_timeout;

    /* History */
    cmd_history_entry_t history[MAX_HISTORY]; /* circular buffer */
    int history_head;                         /* index urmator de scris */
    int history_count;                        /* cate intrari valide */

    /* Activity indicators */
    bool speech_detected; /* VAD - vocea detectata recent */
    bool listening_cmd;   /* in fereastra de 3 sec dupa wake */
} va_state_t;

/* Initializeaza state manager (creaza mutexul) */
void state_init(void);

/* Setters (thread-safe) */
void state_set_lamp(bool on);
void state_set_sensors(float t, float h);
void state_inc_wake_count(void);
void state_inc_commands_recognized(void);
void state_inc_commands_timeout(void);
void state_add_history(int cmd_id, const char *phrase, float prob);
void state_set_speech_detected(bool d);
void state_set_listening_cmd(bool l);

/* Snapshot atomic - copiaza tot starea intr-o structura locala */
void state_snapshot(va_state_t *out);

#endif