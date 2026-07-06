/*
 * commands.h - definirea comenzilor pentru MultiNet
 *
 * Modificarea acestui fisier afecteaza direct setul de comenzi recunoscute.
 * Frazele sunt in engleza (mn7_en e antrenat pe engleza).
 * ID-urile incep de la 1 (0 e rezervat pentru "no command").
 *
 * Reguli pentru fraze bune de comanda:
 *  - Lowercase
 *  - Spatii simple, fara punctuatie
 *  - Minimum 2 cuvinte (frazele scurte dau rate de false positives mari)
 *  - Eviti cuvinte care suna similar la inceputul a doua comenzi
 */

#ifndef VOICE_COMMANDS_H
#define VOICE_COMMANDS_H

typedef struct
{
    int id;
    const char *phrase;
    const char *description; // pentru log
} voice_command_t;

static const voice_command_t k_commands[] = {
    {1, "turn on the light", "LED extern ON"},
    {2, "turn off the light", "LED extern OFF"},
    {3, "blink the light", "LED extern blink"},
    {4, "what is the temperature", "Citire temperatura"},
    {5, "what is the humidity", "Citire umiditate"},
    {6, "tell me a joke", "Joke"},
    {7, "hello", "Greeting"},
};
#define COMMANDS_COUNT (sizeof(k_commands) / sizeof(k_commands[0]))

#endif