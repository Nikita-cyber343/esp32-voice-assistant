/*
 * oled_ui.c - UI layer pentru OLED 128x64
 *
 * Layout:
 *   ┌────────────────────────────┐
 *   │ Voice Assistant      00:00 │  ← header (linie 0-7)
 *   ├────────────────────────────┤
 *   │                            │
 *   │   <stare curenta mare>     │  ← linia 16-39 (continut)
 *   │                            │
 *   ├────────────────────────────┤
 *   │ LAMP:ON  23.5C  W:47 C:38  │  ← footer (linia 48-63)
 *   └────────────────────────────┘
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "oled_ui.h"
#include "ssd1306.h"
#include "state.h"

static const char *TAG = "OLED_UI";
static bool s_have_oled = false;
static TaskHandle_t s_refresh_task = NULL;

/* ----------------------------------------------------------------------------
 * Format helpers
 * -------------------------------------------------------------------------- */
static void format_uptime(int64_t sec, char *out, size_t out_sz)
{
    int h = sec / 3600;
    int m = (sec % 3600) / 60;
    int s = sec % 60;
    if (h > 0)
        snprintf(out, out_sz, "%d:%02d:%02d", h, m, s);
    else
        snprintf(out, out_sz, "%02d:%02d", m, s);
}

/* ----------------------------------------------------------------------------
 * Draw functions (per zone)
 * -------------------------------------------------------------------------- */
static void draw_header(const va_state_t *s)
{
    ssd1306_draw_string(0, 0, "Voice Assistant");
    char uptime_s[16];
    int64_t sec = esp_timer_get_time() / 1000000;
    format_uptime(sec, uptime_s, sizeof(uptime_s));
    /* Aliniere dreapta: text are len * 6 pixeli */
    int len = strlen(uptime_s);
    int x = 128 - len * 6;
    ssd1306_draw_string(x, 0, uptime_s);
    /* Linie separator sub header */
    ssd1306_hline(0, 10, 128, true);
}

static void draw_footer(const va_state_t *s)
{
    /* Linie separator deasupra footer */
    ssd1306_hline(0, 46, 128, true);

    char buf[40];
    /* Rand 1: stare lampa + temperatura */
    snprintf(buf, sizeof(buf), "LAMP:%-3s  %.1fC",
             s->lamp_on ? "ON" : "OFF", s->temperature_c);
    ssd1306_draw_string(0, 50, buf);

    /* Rand 2: stats */
    snprintf(buf, sizeof(buf), "W:%lu  OK:%lu  TO:%lu",
             (unsigned long)s->wake_count,
             (unsigned long)s->commands_recognized,
             (unsigned long)s->commands_timeout);
    ssd1306_draw_string(0, 56, buf);
}

static void draw_content_idle(const va_state_t *s)
{
    if (s->speech_detected)
    {
        ssd1306_draw_string_2x(10, 18, "Speaking");
        ssd1306_draw_string(30, 38, "(say 'Hi, ESP')");
    }
    else
    {
        ssd1306_draw_string_2x(20, 18, "Standby");
        ssd1306_draw_string(20, 38, "Say 'Hi, ESP'");
    }
}

static void draw_content_listening(const va_state_t *s)
{
    ssd1306_draw_string_2x(2, 14, "Listening");

    /* Progress bar pentru timeout (poate fi imbunatatit cu real ms ramase) */
    ssd1306_draw_string(0, 34, "say a command...");
    /* Bar simbol */
    ssd1306_draw_progress(0, 38, 128, 5, 50); /* fixat 50%, doar vizual */
}

static void draw_content_command_ok(const va_state_t *s, const cmd_history_entry_t *e)
{
    char buf[40];
    /* Bifa pe 2 randuri (simulam check mark cu pixeli) */
    ssd1306_draw_string_2x(0, 14, "OK!");

    /* Comanda recunoscuta - poate fi tăiată dacă e prea lungă */
    snprintf(buf, sizeof(buf), "%.20s", e->phrase);
    ssd1306_draw_string(0, 34, buf);

    /* Prob */
    snprintf(buf, sizeof(buf), "prob:%.2f", e->prob);
    ssd1306_draw_string(0, 42, buf);
}

static void draw_content_default(const va_state_t *s)
{
    /* Fallback - identic cu idle */
    draw_content_idle(s);
}

/* ----------------------------------------------------------------------------
 * Compose ecran complet dupa starea curenta
 * -------------------------------------------------------------------------- */
static void render(void)
{
    if (!s_have_oled)
        return;

    va_state_t s;
    state_snapshot(&s);

    ssd1306_clear();
    draw_header(&s);

    /* Decide ce afisam in zona de continut */
    if (s.listening_cmd)
    {
        draw_content_listening(&s);
    }
    else if (s.history_count > 0)
    {
        /* Daca ultima comanda e recenta (< 2 sec), o afisam ca rezultat */
        int last_idx = (s.history_head - 1 + MAX_HISTORY) % MAX_HISTORY;
        const cmd_history_entry_t *e = &s.history[last_idx];
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (e->cmd_id > 0 && (now_ms - e->timestamp_ms) < 2000)
        {
            draw_content_command_ok(&s, e);
        }
        else
        {
            draw_content_idle(&s);
        }
    }
    else
    {
        draw_content_default(&s);
    }

    draw_footer(&s);
    ssd1306_refresh();
}

/* ----------------------------------------------------------------------------
 * Task de refresh - asteapta notificari + redeseneaza periodic
 * -------------------------------------------------------------------------- */
static void oled_task(void *arg)
{
    /* Update periodic la 250ms (pentru uptime + tranzitii) */
    const TickType_t period = pdMS_TO_TICKS(250);
    while (1)
    {
        render();
        /* Asteapta notificare sau timeout */
        ulTaskNotifyTake(pdTRUE, period);
    }
}

/* ----------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */
bool oled_ui_init(int sda, int scl)
{
    s_have_oled = ssd1306_init(sda, scl);
    if (!s_have_oled)
    {
        ESP_LOGW(TAG, "OLED neactivat. Sistemul continua fara display.");
        return false;
    }

    /* Splash de boot */
    ssd1306_clear();
    ssd1306_draw_string_2x(0, 8, "Voice");
    ssd1306_draw_string_2x(0, 28, "Assistant");
    ssd1306_draw_string(20, 52, "ESP32-S3 v1.0");
    ssd1306_refresh();
    vTaskDelay(pdMS_TO_TICKS(1200));

    xTaskCreatePinnedToCore(oled_task, "oled", 4096, NULL, 3, &s_refresh_task, 0);
    ESP_LOGI(TAG, "OLED UI pornit");
    return true;
}

void oled_ui_request_refresh(void)
{
    if (s_refresh_task)
        xTaskNotifyGive(s_refresh_task);
}