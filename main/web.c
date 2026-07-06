// web.c — server HTTP + WebSocket pentru dashboard
#include "web.h"
#include "webpage.h" // INDEX_HTML
#include <string.h>
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

// ====== CONFIGUREAZĂ AICI rețeaua WiFi (2.4 GHz) ======
#define WIFI_SSID "DIGI-c2uE"
#define WIFI_PASS "vb_88ENDJ"
#define WIFI_MAX_RETRY 8
// ======================================================

// web.c — server HTTP + WebSocket pentru dashboard
#include "web.h"
#include "webpage.h" // INDEX_HTML
#include <string.h>
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "web";
static httpd_handle_t s_server = NULL;
static SemaphoreHandle_t s_lock; // protejează trimiterile concurente

// ------------------------------------------------------------------ pagina
static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// ----------------------------------------------------- difuzare către clienți
// Trimite un text JSON la toate conexiunile WebSocket deschise.
static void ws_broadcast(const char *json)
{
    if (!s_server || !json)
        return;
    size_t len = strlen(json);

    int fds = CONFIG_LWIP_MAX_LISTENING_TCP; // limită superioară clienți
    int client_fds[16];
    size_t num = sizeof(client_fds) / sizeof(client_fds[0]);
    if (httpd_get_client_list(s_server, &num, client_fds) != ESP_OK)
        return;

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = len,
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < num; i++)
    {
        int fd = client_fds[i];
        if (httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET)
        {
            httpd_ws_send_frame_async(s_server, fd, &frame);
        }
    }
    xSemaphoreGive(s_lock);
    (void)fds;
}

// ------------------------------------------------------ tratarea comenzilor
static void handle_command(const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root)
        return;
    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd))
    {
        const cJSON *val = cJSON_GetObjectItem(root, "val");
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        if (strcmp(cmd->valuestring, "light") == 0 && cJSON_IsString(val))
        {
            ESP_LOGI(TAG, "comandă web: light %s", val->valuestring);
            web_on_light(val->valuestring);
        }
        else if (strcmp(cmd->valuestring, "mode") == 0 && cJSON_IsString(val))
        {
            ESP_LOGI(TAG, "comandă web: mode %s", val->valuestring);
            web_on_mode(val->valuestring);
        }
        else if (strcmp(cmd->valuestring, "ask") == 0 && cJSON_IsString(text))
        {
            ESP_LOGI(TAG, "comandă web: ask \"%s\"", text->valuestring);
            web_on_ask(text->valuestring);
        }
    }
    cJSON_Delete(root);
}

// ------------------------------------------------------------- handler /ws
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    { // handshake
        ESP_LOGI(TAG, "client WebSocket conectat");
        return ESP_OK;
    }
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};
    // aflu lungimea
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK || frame.len == 0)
        return ret;

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf)
        return ESP_ERR_NO_MEM;
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK)
        handle_command((char *)buf);
    free(buf);
    return ret;
}

// ------------------------------------------------------ funcții publice push
void web_broadcast_state(const char *mode, int wakes,
                         const char *last, float temp, float hum)
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"type\":\"state\",\"mode\":\"%s\",\"wakes\":%d,"
             "\"last\":\"%s\",\"temp\":%.1f,\"hum\":%.0f}",
             mode ? mode : "online", wakes, last ? last : "", temp, hum);
    ws_broadcast(json);
}

void web_push_conversation(const char *q, const char *a)
{
    // construiesc JSON cu escape minimal pentru ghilimele
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "conv");
    if (q)
        cJSON_AddStringToObject(root, "q", q);
    if (a)
        cJSON_AddStringToObject(root, "a", a);
    char *json = cJSON_PrintUnformatted(root);
    if (json)
    {
        ws_broadcast(json);
        free(json);
    }
    cJSON_Delete(root);
}

// ------------------------------------------------------------- pornire server
void web_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 7;
    if (httpd_start(&s_server, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "pornirea serverului web a eșuat");
        return;
    }
    httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_get};
    httpd_uri_t ws_uri = {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true};
    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &ws_uri);
    ESP_LOGI(TAG, "server web pornit (http://<ip>/)");
}