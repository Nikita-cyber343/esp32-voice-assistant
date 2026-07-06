/*
 * buzzer.c - implementare buzzer activ pe GPIO16
 *
 * Buzzer-ul are oscilator intern, deci doar HIGH/LOW. Diferentierea
 * sunetelor se face prin durata si pattern (numarul de pulse-uri).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "buzzer.h"

#define BUZZER_GPIO GPIO_NUM_16

static const char *TAG = "BUZZ";
static QueueHandle_t s_q = NULL;

static inline void beep(int duration_ms)
{
    gpio_set_level(BUZZER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(BUZZER_GPIO, 0);
}

static inline void silence(int duration_ms)
{
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
}

static void play_pattern(buzzer_sound_t s)
{
    switch (s)
    {
    case BUZZER_WAKE:
        beep(80);
        break;
    case BUZZER_CMD_OK:
        beep(50);
        silence(50);
        beep(50);
        break;
    case BUZZER_CMD_FAIL:
        beep(200);
        break;
    case BUZZER_BOOT:
        beep(60);
        silence(60);
        beep(60);
        silence(60);
        beep(60);
        break;
    }
}

static void buzzer_task(void *arg)
{
    buzzer_sound_t s;
    while (1)
    {
        if (xQueueReceive(s_q, &s, portMAX_DELAY) == pdTRUE)
        {
            play_pattern(s);
        }
    }
}

void buzzer_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUZZER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(BUZZER_GPIO, 0); /* tace la pornire */

    s_q = xQueueCreate(4, sizeof(buzzer_sound_t));
    xTaskCreatePinnedToCore(buzzer_task, "buzzer", 2048, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "Buzzer initializat pe GPIO%d", BUZZER_GPIO);
}

void buzzer_play(buzzer_sound_t s)
{
    if (s_q)
        xQueueSend(s_q, &s, 0);
}