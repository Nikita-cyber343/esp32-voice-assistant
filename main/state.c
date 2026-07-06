/*
 * state.c — implementare state manager thread-safe
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "state.h"

static va_state_t s_state;
static SemaphoreHandle_t s_mtx;

#define LOCK() xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

void state_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    memset(&s_state, 0, sizeof(s_state));
    s_state.temperature_c = 22.0f;
    s_state.humidity_pct = 50.0f;
}

void state_set_lamp(bool on)
{
    LOCK();
    s_state.lamp_on = on;
    UNLOCK();
}

void state_set_sensors(float t, float h)
{
    LOCK();
    s_state.temperature_c = t;
    s_state.humidity_pct = h;
    UNLOCK();
}

void state_inc_wake_count(void)
{
    LOCK();
    s_state.wake_count++;
    UNLOCK();
}

void state_inc_commands_recognized(void)
{
    LOCK();
    s_state.commands_recognized++;
    UNLOCK();
}

void state_inc_commands_timeout(void)
{
    LOCK();
    s_state.commands_timeout++;
    UNLOCK();
}

void state_add_history(int cmd_id, const char *phrase, float prob)
{
    LOCK();
    cmd_history_entry_t *e = &s_state.history[s_state.history_head];
    e->cmd_id = cmd_id;
    e->prob = prob;
    e->timestamp_ms = esp_timer_get_time() / 1000;
    strncpy(e->phrase, phrase, sizeof(e->phrase) - 1);
    e->phrase[sizeof(e->phrase) - 1] = '\0';

    s_state.history_head = (s_state.history_head + 1) % MAX_HISTORY;
    if (s_state.history_count < MAX_HISTORY)
        s_state.history_count++;
    UNLOCK();
}

void state_set_speech_detected(bool d)
{
    LOCK();
    s_state.speech_detected = d;
    UNLOCK();
}

void state_set_listening_cmd(bool l)
{
    LOCK();
    s_state.listening_cmd = l;
    UNLOCK();
}

void state_snapshot(va_state_t *out)
{
    LOCK();
    memcpy(out, &s_state, sizeof(va_state_t));
    UNLOCK();
}