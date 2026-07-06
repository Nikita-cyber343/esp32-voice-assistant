/*
 * ssd1306.h - driver minimal SSD1306 OLED 128x64 prin I2C
 *
 * Nu depinde de librarii externe (u8g2 etc). Foloseste I2C master nativ
 * ESP-IDF 5.x. Are framebuffer intern (1KB) - tu desenezi in el, apoi
 * dai refresh() ca sa fie afisat.
 */
#ifndef VA_SSD1306_H
#define VA_SSD1306_H

#include <stdint.h>
#include <stdbool.h>

#define SSD1306_W 128
#define SSD1306_H 64

/* Init driver - returneaza true daca OLED-ul raspunde pe I2C */
bool ssd1306_init(int gpio_sda, int gpio_scl);

/* Sterge framebuffer-ul (nu deseneaza nimic pe ecran inca) */
void ssd1306_clear(void);

/* Aplica framebuffer-ul pe ecran fizic */
void ssd1306_refresh(void);

/* Aprinde sau stinge un pixel (0,0 e stanga-sus) */
void ssd1306_set_pixel(int x, int y, bool on);

/* Deseneaza un caracter 6x8 la pozitia (x,y) */
void ssd1306_draw_char(int x, int y, char c);

/* Deseneaza un string. Returneaza latimea desenata in pixeli. */
int ssd1306_draw_string(int x, int y, const char *s);

/* Deseneaza un string folosind font dublat (12x16) */
int ssd1306_draw_string_2x(int x, int y, const char *s);

/* Linie orizontala/verticala simpla */
void ssd1306_hline(int x, int y, int len, bool on);
void ssd1306_vline(int x, int y, int len, bool on);

/* Bar progres orizontal: x,y pozitie, w latime totala, h inaltime, p 0-100% */
void ssd1306_draw_progress(int x, int y, int w, int h, int percent);

#endif