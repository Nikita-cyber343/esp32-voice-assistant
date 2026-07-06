/*
 * dht11.h - driver pentru senzor DHT11 (temperatura + umiditate)
 *
 * Protocol one-wire bit-bang la 5 sec interval. Task background care
 * stocheaza valorile in variabile globale thread-safe (volatile + atomic).
 */
#ifndef VA_DHT11_H
#define VA_DHT11_H

#include <stdint.h>
#include <stdbool.h>

/* Initializeaza GPIO + task de citire periodic */
void dht11_init(int gpio_data);

/* Returneaza ultima valoare valida temperatura (°C) sau -999 daca neinitializat */
float dht11_get_temperature(void);

/* Returneaza ultima valoare valida umiditate (%) sau -999 */
float dht11_get_humidity(void);

/* True daca am avut macar 1 citire reusita de la boot */
bool dht11_has_valid_reading(void);

#endif