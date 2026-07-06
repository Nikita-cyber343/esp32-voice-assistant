/*
 * oled_ui.h - UI layer pentru OLED
 *
 * Functii de inalt nivel pentru afisaj. Toate iau snapshot din state.h
 * intern si redeseneaza tot ecranul (no incremental rendering).
 *
 * Run intr-un task separat care reactioneaza la evenimente.
 */
#ifndef VA_OLED_UI_H
#define VA_OLED_UI_H

#include <stdbool.h>

/* Init driver + lansare task de refresh.
   Returneaza false daca OLED nu detectat (continuam fara display). */
bool oled_ui_init(int sda, int scl);

/* Cere un refresh al ecranului. Apelat dupa fiecare schimbare in state. */
void oled_ui_request_refresh(void);

#endif