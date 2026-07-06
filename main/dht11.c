/*
 * dht11.c - DHT11 one-wire bit-bang driver
 *
 * Protocol DHT11:
 *   1. Host pulls DATA low pentru >=18ms (start signal)
 *   2. Host releases (pull-up trage HIGH)
 *   3. DHT11 raspunde: 80us LOW + 80us HIGH (response)
 *   4. Apoi 40 biti date: fiecare bit are 50us LOW urmat de
 *      - 26-28us HIGH = bit 0
 *      - 70us HIGH = bit 1
 *   5. Datele: 16 biti humidity (high byte + low byte) +
 *              16 biti temperature + 8 biti checksum
 *
 * Timing critic - dezactivam intreruperile pe perioada citirii (~5ms).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h" /* ets_delay_us */

#include "dht11.h"

#define READ_INTERVAL_MS 5000 /* DHT11 are min 1 Hz rate */

static const char *TAG = "DHT11";
static int s_gpio = -1;
static volatile float s_temperature = -999.0f;
static volatile float s_humidity = -999.0f;
static volatile bool s_has_valid = false;

/* Asteapta o tranzitie a pinului si returneaza durata pana la tranzitie (us)
 * sau -1 daca timeout. */
static int wait_pin_change(int expected_level, int timeout_us)
{
    int elapsed = 0;
    while (gpio_get_level(s_gpio) != expected_level)
    {
        if (elapsed >= timeout_us)
            return -1;
        ets_delay_us(1);
        elapsed++;
    }
    return elapsed;
}

/* Citeste 40 biti de la DHT11. Returneaza true daca checksum-ul e corect. */
static bool dht11_read_raw(uint8_t *out)
{
    /* Start signal: pin LOW pentru >=18ms */
    gpio_set_direction(s_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(s_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Release line - pull-up trage HIGH ~40us */
    gpio_set_level(s_gpio, 1);
    ets_delay_us(40);

    /* Switch la input pentru a citi raspunsul */
    gpio_set_direction(s_gpio, GPIO_MODE_INPUT);

    /* Timing critic - intreruperi off */
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);

    /* DHT11 trage LOW ~80us */
    if (wait_pin_change(0, 100) < 0)
    {
        portEXIT_CRITICAL(&mux);
        return false;
    }
    /* DHT11 trage HIGH ~80us */
    if (wait_pin_change(1, 100) < 0)
    {
        portEXIT_CRITICAL(&mux);
        return false;
    }
    /* DHT11 trage LOW (inceputul primului bit) */
    if (wait_pin_change(0, 100) < 0)
    {
        portEXIT_CRITICAL(&mux);
        return false;
    }

    /* Citim 40 biti */
    for (int i = 0; i < 40; i++)
    {
        /* Start bit: ~50us LOW (deja suntem in LOW) */
        if (wait_pin_change(1, 80) < 0)
        {
            portEXIT_CRITICAL(&mux);
            return false;
        }
        /* HIGH duration: 26-28us = '0', 70us = '1' */
        int duration = wait_pin_change(0, 90);
        if (duration < 0)
        {
            portEXIT_CRITICAL(&mux);
            return false;
        }
        /* Threshold ~40us: peste = '1', sub = '0' */
        if (duration > 40)
        {
            out[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    portEXIT_CRITICAL(&mux);

    /* Checksum: byte 4 = (byte 0 + byte 1 + byte 2 + byte 3) & 0xFF */
    uint8_t cs = out[0] + out[1] + out[2] + out[3];
    return (cs & 0xFF) == out[4];
}

static void dht11_task(void *arg)
{
    /* Pull-up intern pe linie (modulul DHT11 cu PCB are si extern, dar redundanta nu strica) */
    gpio_set_pull_mode(s_gpio, GPIO_PULLUP_ONLY);

    /* DHT11 are nevoie de 1 secunda de stabilizare la pornire */
    vTaskDelay(pdMS_TO_TICKS(1500));

    while (1)
    {
        uint8_t raw[5] = {0};
        if (dht11_read_raw(raw))
        {
            /* DHT11 format: H_int.H_dec T_int.T_dec
             * Pentru DHT11, partea decimala e mereu 0 (rezolutie 1°C / 1%).
             * Pentru DHT22 (compatibil), e diferit - aici tratam ca DHT11 simplu. */
            float h = (float)raw[0] + (float)raw[1] / 10.0f;
            float t = (float)raw[2] + (float)raw[3] / 10.0f;
            if (h >= 0 && h <= 100 && t >= -40 && t <= 80)
            {
                s_humidity = h;
                s_temperature = t;
                s_has_valid = true;
                ESP_LOGI(TAG, "T=%.1f°C H=%.1f%%", t, h);
            }
            else
            {
                ESP_LOGW(TAG, "Citire absurda: T=%.1f H=%.1f (ignorata)", t, h);
            }
        }
        else
        {
            ESP_LOGW(TAG, "Citire DHT11 esuata (checksum sau timeout)");
        }
        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
    }
}

void dht11_init(int gpio_data)
{
    s_gpio = gpio_data;
    gpio_set_pull_mode(s_gpio, GPIO_PULLUP_ONLY);
    xTaskCreatePinnedToCore(dht11_task, "dht11", 3072, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "DHT11 initializat pe GPIO%d (citire la %d ms)",
             gpio_data, READ_INTERVAL_MS);
}

float dht11_get_temperature(void) { return s_temperature; }
float dht11_get_humidity(void) { return s_humidity; }
bool dht11_has_valid_reading(void) { return s_has_valid; }