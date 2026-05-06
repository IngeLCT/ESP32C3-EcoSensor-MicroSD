#include "captive_manager.h"
#include "wifi_store.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi_manager";

static captive_manager_cfg_t g_cfg;
static captive_state_t g_state = CAP_STATE_IDLE;
static httpd_handle_t g_server = NULL;
static esp_netif_t *g_ap_netif = NULL;
static esp_netif_t *g_sta_netif = NULL;

static bool g_sta_have_ip = false;
static bool g_using_saved = false;
static int  g_connect_attempts = 0;
static int64_t g_boot_time_ms = 0;

static void restart_later_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void set_state(captive_state_t st);
static esp_err_t start_ap(void);
static esp_err_t start_http(void);
static void scan_start(void);
static void connect_sta(const char *ssid, const char *pass, bool from_saved);
static void shutdown_ap_http(void);
static void start_mdns_service(void);
static bool in_boot_grace_window(void);
static void start_with_saved_or_manager(void);

const char* captive_manager_state_str(captive_state_t st) {
    switch(st){
        case CAP_STATE_IDLE: return "IDLE";
        case CAP_STATE_PREP: return "PREP";
        case CAP_STATE_SCAN: return "SCAN";
        case CAP_STATE_CONNECTING: return "CONNECTING";
        case CAP_STATE_WAIT_LOGIN: return "WAIT_LOGIN";
        case CAP_STATE_VERIFY: return "VERIFY";
        case CAP_STATE_OPERATIONAL: return "OPERATIONAL";
        case CAP_STATE_RECAPTIVE: return "RECAPTIVE";
        default: return "UNKNOWN";
    }
}

captive_state_t captive_manager_get_state(void){
    return g_state;
}

static void set_state(captive_state_t st) {
    if (g_state != st) {
        ESP_LOGI(TAG, "STATE: %s -> %s", captive_manager_state_str(g_state), captive_manager_state_str(st));
        g_state = st;
    }
}

bool captive_manager_using_saved(void) {
    return g_using_saved;
}

esp_err_t captive_manager_init(const captive_manager_cfg_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    g_cfg = *cfg;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    g_ap_netif  = esp_netif_create_default_wifi_ap();
    g_sta_netif = esp_netif_create_default_wifi_sta();
    (void)g_ap_netif;
    (void)g_sta_netif;

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    g_boot_time_ms = esp_timer_get_time() / 1000;
    set_state(CAP_STATE_IDLE);
    return ESP_OK;
}

static bool in_boot_grace_window(void) {
    if (g_cfg.boot_grace_ms <= 0) return false;
    int64_t now_ms = esp_timer_get_time() / 1000;
    return (now_ms - g_boot_time_ms) < g_cfg.boot_grace_ms;
}

static void start_with_saved_or_manager(void) {
    if (wifi_store_has_credentials()) {
        char ssid[33], pass[65];
        if (wifi_store_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
            ESP_LOGI(TAG, "Credenciales guardadas encontradas: SSID=%s", ssid);
            connect_sta(ssid, pass, true);
            return;
        }
    }

    set_state(CAP_STATE_PREP);
    ESP_ERROR_CHECK(start_ap());
    ESP_ERROR_CHECK(start_http());
    set_state(CAP_STATE_SCAN);
    scan_start();
}

esp_err_t captive_manager_start(void) {
    if (g_state != CAP_STATE_IDLE) return ESP_ERR_INVALID_STATE;
    start_with_saved_or_manager();
    return ESP_OK;
}

static esp_err_t start_ap(void) {
    wifi_config_t ap_cfg = {0};
    snprintf((char*)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", g_cfg.ap_ssid);
    snprintf((char*)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), "%s", g_cfg.ap_pass);
    ap_cfg.ap.ssid_len = strlen((char*)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = strlen(g_cfg.ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_LOGI(TAG, "AP started SSID=%s", ap_cfg.ap.ssid);
    start_mdns_service();
    return ESP_OK;
}

static void connect_sta(const char *ssid, const char *pass, bool from_saved) {
    wifi_config_t sta_cfg = {0};
    snprintf((char*)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%s", ssid);
    snprintf((char*)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", pass ? pass : "");
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    set_state(CAP_STATE_CONNECTING);
    g_sta_have_ip = false;
    g_using_saved = from_saved;
    g_connect_attempts = 0;
    ESP_LOGI(TAG, "(STA) Connecting to SSID=%s saved=%d", sta_cfg.sta.ssid, from_saved);
    ESP_ERROR_CHECK(esp_wifi_connect());
    start_mdns_service();
}

void captive_manager_notify_sta_got_ip(void) {
    g_sta_have_ip = true;
    g_connect_attempts = 0;
    shutdown_ap_http();
    set_state(CAP_STATE_OPERATIONAL);
    ESP_LOGI(TAG, "STA connected and operational");
}

void captive_manager_notify_sta_disconnected(int reason_code) {
    ESP_LOGW(TAG, "STA disconnected (reason=%d) state=%s saved=%d attempts=%d",
             reason_code, captive_manager_state_str(g_state), g_using_saved, g_connect_attempts);
    g_sta_have_ip = false;

    if (g_state == CAP_STATE_CONNECTING || g_state == CAP_STATE_OPERATIONAL) {
        g_connect_attempts++;
        bool in_grace = in_boot_grace_window();
        bool under_limit = (g_connect_attempts < g_cfg.conn_max_attempts);

        if (under_limit || in_grace) {
            vTaskDelay(pdMS_TO_TICKS(g_cfg.conn_retry_delay_ms));
            esp_wifi_connect();
            return;
        }

        ESP_LOGW(TAG, "Sin conexión tras reintentos. Volviendo al Wi-Fi manager");
        captive_manager_enter_recaptive();
    }
}

esp_err_t captive_manager_enter_recaptive(void) {
    ESP_LOGW(TAG, "Entering Wi-Fi manager mode");
    shutdown_ap_http();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    g_sta_have_ip = false;
    g_using_saved = false;
    g_connect_attempts = 0;

    set_state(CAP_STATE_RECAPTIVE);
    ESP_ERROR_CHECK(start_ap());
    if (!g_server) {
        ESP_ERROR_CHECK(start_http());
    }
    set_state(CAP_STATE_SCAN);
    scan_start();
    return ESP_OK;
}

static void scan_start(void) {
    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE
    };
    esp_wifi_scan_start(&sc, false);
}

static esp_err_t favicon_get(httpd_req_t *r)
{
    httpd_resp_set_status(r, "204 No Content");
    httpd_resp_set_type(r, "image/x-icon");
    return httpd_resp_send(r, NULL, 0);
}

static esp_err_t root_get(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store, no-cache, must-revalidate, proxy-revalidate");
    httpd_resp_set_hdr(r, "Pragma", "no-cache");
    httpd_resp_set_hdr(r, "Expires", "0");
    httpd_resp_set_hdr(r, "Connection", "close");

    char ubic[64] = {0};
    if (wifi_store_get_location(ubic, sizeof(ubic)) != ESP_OK) {
        ubic[0] = '\0';
    }

    char ubic_escaped[96];
    size_t j = 0;
    for (size_t i = 0; ubic[i] && j < sizeof(ubic_escaped) - 1; ++i) {
        if (ubic[i] == '"') {
            if (j + 6 < sizeof(ubic_escaped)) {
                memcpy(&ubic_escaped[j], "&quot;", 6);
                j += 6;
            } else break;
        } else if (ubic[i] == '&') {
            if (j + 5 < sizeof(ubic_escaped)) {
                memcpy(&ubic_escaped[j], "&amp;", 5);
                j += 5;
            } else break;
        } else if (ubic[i] == '<') {
            if (j + 4 < sizeof(ubic_escaped)) {
                memcpy(&ubic_escaped[j], "&lt;", 4);
                j += 4;
            } else break;
        } else if (ubic[i] == '>') {
            if (j + 4 < sizeof(ubic_escaped)) {
                memcpy(&ubic_escaped[j], "&gt;", 4);
                j += 4;
            } else break;
        } else {
            ubic_escaped[j++] = ubic[i];
        }
    }
    ubic_escaped[j] = '\0';

    const char *head =
        "<!doctype html><html lang=\"es\"><head>"
        "<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>EcoSensor Wi-Fi Manager</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Helvetica,Arial,sans-serif;margin:0;background:#f6f7fb;color:#111}"
        ".wrap{max-width:720px;margin:24px auto;padding:16px}"
        ".card{background:#fff;border-radius:16px;box-shadow:0 6px 18px rgba(0,0,0,.08);padding:20px}"
        "h1{font-size:20px;margin:0 0 12px}"
        "label{display:block;font-weight:600;margin:16px 0 6px}"
        "select,input{width:100%;box-sizing:border-box;padding:10px 12px;border:1px solid #dcdfe6;border-radius:10px;font-size:14px}"
        "button,input[type=submit]{margin-top:18px;border:0;border-radius:12px;padding:12px 16px;cursor:pointer;font-weight:600}"
        "input[type=submit]{background:#111;color:#fff}"
        "</style></head><body><div class=\"wrap\"><div class=\"card\">"
        "<h1>EcoSensor</h1>"
        "<h1>Wi-Fi Manager</h1>"
        "<form id=\"wifiForm\">"
        "<label for=\"ssid\">SSID</label>"
        "<select id=\"ssid\" name=\"ssid\" required><option value=\"\" disabled selected>Cargando redes...</option></select>"
        "<label for=\"pass\">Contraseña</label>"
        "<input type=\"password\" id=\"pass\" name=\"pass\">"
        "<label for=\"ubic\">Ubicación</label>"
        "<input type=\"text\" id=\"ubic\" name=\"ubic\" placeholder=\"Ej. Taller / Sala 1\" value=\"";

    const char *tail =
        "\">"
        "<input type=\"submit\" value=\"Guardar y reiniciar\">"
        "</form>"
        "</div></div>"
        "<script>\"use strict\";"
        "async function loadNetworks(){"
        "try{"
        "const r=await fetch('/scan');"
        "const j=await r.json();"
        "const aps=Array.isArray(j)?j:(Array.isArray(j.networks)?j.networks:[]);"
        "const sel=document.getElementById('ssid');"
        "sel.innerHTML='';"
        "if(aps.length){"
        "for(const ap of aps){"
        "const o=document.createElement('option');"
        "const isOpen=!!ap.open;"
        "o.value=ap.ssid||'';"
        "o.setAttribute('data-open',isOpen?'1':'0');"
        "o.textContent=(ap.ssid||'(oculta)')+(isOpen?' (abierta)':'');"
        "sel.appendChild(o);"
        "}"
        "sel.selectedIndex=0;applyPassDisable();"
        "}else{sel.innerHTML='<option value=\"\">(No se encontraron redes)</option>';document.getElementById('pass').disabled=false;}"
        "}catch(e){console.error('Error cargando redes:',e);}"
        "}"
        "function applyPassDisable(){"
        "const sel=document.getElementById('ssid');"
        "const pass=document.getElementById('pass');"
        "const opt=sel&&sel.options[sel.selectedIndex];"
        "const isOpen=!!(opt&&opt.getAttribute('data-open')==='1');"
        "pass.disabled=isOpen;"
        "if(isOpen){pass.value='';pass.placeholder='Esta red no requiere contraseña';}else{pass.placeholder=' ';}"
        "}"
        "document.getElementById('ssid').addEventListener('change',applyPassDisable);"
        "document.getElementById('wifiForm').addEventListener('submit',async(ev)=>{"
        "ev.preventDefault();"
        "const ssid=document.getElementById('ssid').value||'';"
        "const pass=document.getElementById('pass').value||'';"
        "const ubic=document.getElementById('ubic').value||'';"
        "try{"
        "const resp=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass,ubic})});"
        "const txt=await resp.text();"
        "alert(txt||'Guardado. Reiniciando...');"
        "}catch(e){alert('Error guardando: '+(e&&e.message?e.message:e));}"
        "});"
        "window.addEventListener('load',()=>{loadNetworks();});"
        "</script></body></html>";

    size_t total = strlen(head) + strlen(ubic_escaped) + strlen(tail) + 1;
    char *page = (char*)malloc(total);
    if (!page) {
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    strcpy(page, head);
    strcat(page, ubic_escaped);
    strcat(page, tail);

    esp_err_t er = httpd_resp_send(r, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return er;
}

static esp_err_t save_post(httpd_req_t *r) {
    char buf[512];
    int len = httpd_req_recv(r, buf, sizeof(buf)-1);
    if (len <= 0) return httpd_resp_send_err(r, 400, "empty");
    buf[len] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(r, 400, "json");

    cJSON *js = cJSON_GetObjectItem(root, "ssid");
    cJSON *jp = cJSON_GetObjectItem(root, "pass");
    cJSON *ju = cJSON_GetObjectItem(root, "ubic");
    if (!cJSON_IsString(js)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(r, 400, "ssid?");
    }

    const char *ssid = js->valuestring;
    const char *pass = (jp && cJSON_IsString(jp)) ? jp->valuestring : "";

    wifi_store_save(ssid, pass);
    if (ju && cJSON_IsString(ju)) {
        wifi_store_set_location(ju->valuestring);
    }

    httpd_resp_sendstr(r, "Datos guardados. El dispositivo se reiniciará para conectar a la red.");
    xTaskCreate(restart_later_task, "rst_later", 2048, NULL, 5, NULL);

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t scan_get(httpd_req_t *r) {
    wifi_mode_t old_mode;
    esp_wifi_get_mode(&old_mode);
    if (old_mode != WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t sc = { .show_hidden = false };
    esp_wifi_scan_start(&sc, true);

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > (uint16_t)g_cfg.max_scan_aps) ap_num = (uint16_t)g_cfg.max_scan_aps;

    wifi_ap_record_t *list = calloc(ap_num ? ap_num : 1, sizeof(wifi_ap_record_t));
    if (!list) {
        if (old_mode != WIFI_MODE_APSTA) esp_wifi_set_mode(old_mode);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }

    if (ap_num > 0) {
        esp_wifi_scan_get_ap_records(&ap_num, list);
    }

    httpd_resp_set_type(r, "application/json");
    httpd_resp_send_chunk(r, "{\"networks\":[", -1);
    for (int i = 0; i < ap_num; i++) {
        char buf[128];
        int open = (list[i].authmode == WIFI_AUTH_OPEN) ? 1 : 0;
        snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\",\"open\":%d}", (char*)list[i].ssid, open);
        httpd_resp_send_chunk(r, buf, -1);
        if (i < ap_num - 1) httpd_resp_send_chunk(r, ",", -1);
    }
    httpd_resp_send_chunk(r, "]}", -1);
    httpd_resp_send_chunk(r, NULL, 0);

    free(list);
    if (old_mode != WIFI_MODE_APSTA) esp_wifi_set_mode(old_mode);
    return ESP_OK;
}

static esp_err_t status_get(httpd_req_t *r) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", captive_manager_state_str(g_state));
    cJSON_AddBoolToObject(root, "sta_ip", g_sta_have_ip);
    cJSON_AddBoolToObject(root, "using_saved", g_using_saved);
    cJSON_AddNumberToObject(root, "conn_attempts", g_connect_attempts);
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, out);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t wifi_clear_delete(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_sendstr(r, "Credenciales Wi-Fi borradas. El dispositivo se reiniciará en segundos.");
    wifi_store_clear();
    xTaskCreate(restart_later_task, "rst_later", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t wifi_clear_get(httpd_req_t *r) {
    return wifi_clear_delete(r);
}

static httpd_uri_t uri_favicon          = { .uri="/favicon.ico", .method=HTTP_GET,    .handler=favicon_get };
static httpd_uri_t uri_root             = { .uri="/",            .method=HTTP_GET,    .handler=root_get };
static httpd_uri_t uri_scan             = { .uri="/scan",        .method=HTTP_GET,    .handler=scan_get };
static httpd_uri_t uri_save             = { .uri="/save",        .method=HTTP_POST,   .handler=save_post };
static httpd_uri_t uri_status           = { .uri="/status",      .method=HTTP_GET,    .handler=status_get };
static httpd_uri_t uri_wifi_clr         = { .uri="/wifi/clear",  .method=HTTP_DELETE, .handler=wifi_clear_delete };
static httpd_uri_t uri_wifi_clr_get     = { .uri="/wifi/clear",  .method=HTTP_GET,    .handler=wifi_clear_get };

static esp_err_t start_http(void) {
    if (g_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 2;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    if (httpd_start(&g_server, &cfg) == ESP_OK) {
        httpd_register_uri_handler(g_server, &uri_root);
        httpd_register_uri_handler(g_server, &uri_favicon);
        httpd_register_uri_handler(g_server, &uri_scan);
        httpd_register_uri_handler(g_server, &uri_save);
        httpd_register_uri_handler(g_server, &uri_status);
        httpd_register_uri_handler(g_server, &uri_wifi_clr);
        httpd_register_uri_handler(g_server, &uri_wifi_clr_get);
        return ESP_OK;
    }

    return ESP_FAIL;
}

static void shutdown_ap_http(void) {
    if (g_server) {
        httpd_stop(g_server);
        g_server = NULL;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
}

static void start_mdns_service(void) {
    static bool mdns_started = false;
    if (mdns_started) return;

    if (mdns_init() == ESP_OK) {
        const char *hostname = (g_cfg.mdns_hostname && g_cfg.mdns_hostname[0]) ? g_cfg.mdns_hostname : "ecosensor";
        mdns_hostname_set(hostname);
        mdns_instance_name_set(hostname);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        mdns_started = true;
        ESP_LOGI(TAG, "mDNS started as %s.local", hostname);
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }
}
