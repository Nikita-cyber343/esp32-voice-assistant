// web.c — server HTTP + WebSocket pentru dashboard
#include "web.h"
#include "webpage.h" // INDEX_HTML
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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
#define WIFI_SSID "iPhone-Nekit"
#define WIFI_PASS "12345678"
#define WIFI_MAX_RETRY 8
// ======================================================

static const char *TAG = "web";
static httpd_handle_t s_server = NULL;
static SemaphoreHandle_t s_lock; // protejează trimiterile concurente

/* ============================ WiFi station ============================ */
static EventGroupHandle_t s_wifi_ev;
#define WIFI_OK BIT0
#define WIFI_FAIL BIT1
static int s_retry = 0;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "deconectat (reason=%d)", d->reason);
        if (s_retry < WIFI_MAX_RETRY)
        {
            s_retry++;
            ESP_LOGW(TAG, "reconectare WiFi (%d/%d)", s_retry, WIFI_MAX_RETRY);
            esp_wifi_connect();
        }
        else
        {
            xEventGroupSetBits(s_wifi_ev, WIFI_FAIL);
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP primit: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_wifi_ev, WIFI_OK);
    }
}

/* Initializeaza WiFi in mod statie si asteapta conexiunea (max 20 s).
 * Intoarce true doar daca placa a primit IP.
 * esp_netif_init() porneste firul TCP/IP (lwIP) — fara el, orice socket
 * (inclusiv cel al serverului HTTP) cade cu assert "Invalid mbox". */
static bool wifi_init_sta(void)
{
    s_wifi_ev = xEventGroupCreate();

    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, NULL, NULL);

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); /* latenta mai mica, conexiune mai stabila */

    ESP_LOGI(TAG, "conectare la \"%s\"...", WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_ev, WIFI_OK | WIFI_FAIL,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    return (bits & WIFI_OK) != 0;
}

// ------------------------------------------------------------------ pagina
static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// ----------------------------------------------------- difuzare către clienți
// Trimite un text JSON la toate conexiunile WebSocket deschise.
/* Trimitere asincrona corecta: expediata din contextul serverului httpd. */
typedef struct
{
    httpd_handle_t hd;
    int fd;
    char *json; /* copie proprie — eliberata dupa trimitere */
} ws_send_job_t;

static void ws_send_job(void *arg)
{
    ws_send_job_t *job = (ws_send_job_t *)arg;
    httpd_ws_frame_t frame = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)job->json,
        .len = strlen(job->json),
    };
    httpd_ws_send_frame_async(job->hd, job->fd, &frame);
    free(job->json);
    free(job);
}

static void ws_broadcast(const char *json)
{
    if (!s_server || !json)
        return;

    int client_fds[16];
    size_t num = sizeof(client_fds) / sizeof(client_fds[0]);
    if (httpd_get_client_list(s_server, &num, client_fds) != ESP_OK)
        return;

    for (size_t i = 0; i < num; i++)
    {
        int fd = client_fds[i];
        if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET)
            continue;
        ws_send_job_t *job = malloc(sizeof(*job));
        if (!job)
            continue;
        job->hd = s_server;
        job->fd = fd;
        job->json = strdup(json);
        if (!job->json)
        {
            free(job);
            continue;
        }
        if (httpd_queue_work(s_server, ws_send_job, job) != ESP_OK)
        {
            free(job->json);
            free(job);
        }
    }
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
    if (!wifi_init_sta())
    {
        ESP_LOGE(TAG, "WiFi indisponibil — serverul web nu pornește.");
        return;
    }
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