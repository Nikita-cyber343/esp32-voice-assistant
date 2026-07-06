// web.h — server web + WebSocket pentru dashboard-ul asistentului vocal
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

   // Pornește serverul HTTP + WebSocket (apelat o singură dată, după Wi-Fi).
   void web_start(void);

   // Trimite starea curentă către toate paginile conectate.
   // mode: "online"/"offline"; last: ultima comandă; temp/hum: valori senzor.
   void web_broadcast_state(const char *mode, int wakes,
                            const char *last, float temp, float hum);

   // Adaugă o pereche întrebare/răspuns în istoricul din browser (mod online).
   void web_push_conversation(const char *q, const char *a);

   // ---- Callback-uri implementate în main.c (apelate la comenzi din browser) ----
   void web_on_light(const char *val); // "on" / "off" / "blink"
   void web_on_mode(const char *val);  // "online" / "offline"
   void web_on_ask(const char *text);  // întrebare scrisă în interfață

#ifdef __cplusplus
}
#endif