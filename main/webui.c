// Talon — HTTP server: serves the controller web UI and the /api endpoints
// that drive the XID input state.
//
//   GET /                        the controller page (setup page in AP mode)
//   GET /setup                   WiFi provisioning page (scan / join / WPS)
//   GET /api/btn?b=NAME&v=0|1    hold / release a control
//   GET /api/axis?a=NAME&v=INT   set an analog control (axes -32768..32767)
//   GET /api/press?b=NAME[&ms=N] tap: press, wait N ms (default 120), release
//   GET /api/release             release everything
//   GET /api/status              JSON diagnostics
//   GET /api/scan                JSON list of visible networks
//   GET /api/wifi/save?ssid&pass save credentials + join (leaves setup AP)
//   GET /api/wifi/forget         erase credentials, back to setup AP
//   GET /api/wps/start           WPS push-button join
//
// Everything is GET so a plain curl can drive the pad from scripts. In setup
// (SoftAP) mode every unknown path 302s to the setup page — combined with the
// captive DNS in wifi_net.c that makes phones pop the portal open.
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "tusb.h"
#include "talon.h"
#include "usb_owner.h"
#include "talon_igr.h"
#include "wifi_net.h"
#include "bt_host.h"

static const char *TAG = "talon.web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[]   asm("_binary_setup_html_end");
extern const char ota_html_start[]   asm("_binary_ota_html_start");
extern const char ota_html_end[]     asm("_binary_ota_html_end");

void ota_register(httpd_handle_t srv);

static esp_err_t ota_page_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, ota_html_start, ota_html_end - ota_html_start - 1);
}

static esp_err_t setup_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start - 1);
}

static esp_err_t root_get(httpd_req_t *req) {
    // While provisioning, the root IS the setup page (captive portals land here).
    if (wifi_net_mode() == TALON_NET_AP_SETUP) return setup_get(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

// Captive portal: in setup mode, every unknown URL (Android /generate_204,
// iOS /hotspot-detect.html, Windows /connecttest.txt, ...) redirects to the
// setup page so the phone's sign-in sheet opens it.
static esp_err_t err_404(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    if (wifi_net_mode() == TALON_NET_AP_SETUP) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, "not found");
}

// httpd_query_key_value does not URL-decode; passwords need it.
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') { *o++ = ' '; }
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = '\0';
}

// Pull ?key= out of the query string; returns false if absent.
// Some ESP-IDF/httpd configurations can return an empty query string from
// httpd_req_get_url_query_str(), so fall back to parsing req->uri directly.
static bool qs_value(httpd_req_t *req, const char *key, char *out, size_t outlen) {
    if (!req || !key || !out || outlen == 0) return false;

    // Parse req->uri first.  This preserves percent-encoded values exactly as
    // received; httpd_req_get_url_query_str() may partially decode malformed or
    // percent-encoded values on some ESP-IDF/httpd versions.  Callers that need
    // decoding (such as Bluetooth addresses) call url_decode() explicitly.

    // Direct parser: req->uri contains the original path + query.
    // Parse key/value pairs without modifying req->uri.
    const char *q = strchr(req->uri, '?');
    if (!q) return false;
    q++;

    const size_t keylen = strlen(key);
    while (*q) {
        const char *eq = strchr(q, '=');
        if (!eq) break;
        const char *amp = strchr(eq + 1, '&');
        size_t klen = (size_t)(eq - q);
        if (klen == keylen && strncmp(q, key, keylen) == 0) {
            size_t vlen = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
            if (vlen >= outlen) vlen = outlen - 1;
            memcpy(out, eq + 1, vlen);
            out[vlen] = '\0';
            return true;
        }
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

static esp_err_t api_ok(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_bad(httpd_req_t *req, const char *why) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", why);
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t btn_get(httpd_req_t *req) {
    char b[16], v[12];
    if (!qs_value(req, "b", b, sizeof(b))) return api_bad(req, "missing b");
    int val = qs_value(req, "v", v, sizeof(v)) ? atoi(v) : 1;
    if (!talon_set_control(b, val)) return api_bad(req, "unknown control");
    return api_ok(req);
}

static esp_err_t axis_get(httpd_req_t *req) {
    char a[16], v[12];
    if (!qs_value(req, "a", a, sizeof(a)) || !qs_value(req, "v", v, sizeof(v)))
        return api_bad(req, "missing a/v");
    if (!talon_set_control(a, atoi(v))) return api_bad(req, "unknown control");
    return api_ok(req);
}

static esp_err_t press_get(httpd_req_t *req) {
    char b[16], ms[12];
    if (!qs_value(req, "b", b, sizeof(b))) return api_bad(req, "missing b");
    int hold = qs_value(req, "ms", ms, sizeof(ms)) ? atoi(ms) : 120;
    if (hold < 20) hold = 20;
    if (hold > 5000) hold = 5000;
    if (!talon_set_control(b, 1)) return api_bad(req, "unknown control");
    vTaskDelay(pdMS_TO_TICKS(hold));
    talon_set_control(b, 0);
    return api_ok(req);
}

static esp_err_t release_get(httpd_req_t *req) {
    talon_reset_controls();
    return api_ok(req);
}

// Bulk state for gamepad forwarding: one request carries the whole report.
// v = 13 comma-separated ints: digital,a,b,x,y,black,white,lt,rt,lx,ly,rx,ry
static esp_err_t state_get(httpd_req_t *req) {
    char v[160];
    if (!qs_value(req, "v", v, sizeof(v))) return api_bad(req, "missing v");
    url_decode(v);
    int vals[13] = { 0 };
    int n = 0;
    for (char *p = v; n < 13; n++) {
        vals[n] = atoi(p);
        char *c = strchr(p, ',');
        if (!c) { n++; break; }
        p = c + 1;
    }
    if (n != 13) return api_bad(req, "need 13 values");
    talon_set_state_all(vals);
    return api_ok(req);
}

// Cerbios IGR shortcut: hold a default reset/reload/shutdown combo.
static esp_err_t igr_get(httpd_req_t *req) {
    char a[16];
    if (!qs_value(req, "a", a, sizeof(a))) return api_bad(req, "missing a");
    if (!talon_igr_trigger(a)) return api_bad(req, "unknown igr action");
    return api_ok(req);
}

// ---- WiFi provisioning endpoints ------------------------------------------

static esp_err_t scan_get(httpd_req_t *req) {
    static char json[2048];          // one at a time; httpd workers serialize on this
    wifi_net_scan_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t wifi_save_get(httpd_req_t *req) {
    char ssid[64] = "", pass[96] = "";
    if (!qs_value(req, "ssid", ssid, sizeof(ssid))) return api_bad(req, "missing ssid");
    qs_value(req, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);
    if (!ssid[0]) return api_bad(req, "empty ssid");
    wifi_net_save_creds(ssid, pass);
    return api_ok(req);              // reply goes out before the AP drops (1.5 s)
}

static esp_err_t wifi_forget_get(httpd_req_t *req) {
    esp_err_t r = api_ok(req);       // answer first: the STA link is about to drop
    wifi_net_forget();
    return r;
}

static esp_err_t wps_start_get(httpd_req_t *req) {
    esp_err_t r = api_ok(req);       // answer first: the setup AP drops for WPS
    wifi_net_start_wps();
    return r;
}

// ---- Bluetooth controller endpoints ---------------------------------------

static esp_err_t bt_scan_get(httpd_req_t *req) {
    static char json[2048];
    bt_host_scan_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t bt_connect_get(httpd_req_t *req) {
    char addr[40];
    if (!qs_value(req, "addr", addr, sizeof(addr))) return api_bad(req, "missing addr");
    url_decode(addr);
    ESP_LOGI(TAG, "BLE connect request addr='%s' uri='%s'", addr, req->uri);
    if (!bt_host_connect(addr)) return api_bad(req, "bad addr");
    return api_ok(req);
}

static esp_err_t bt_forget_get(httpd_req_t *req) {
    bt_host_forget();
    return api_ok(req);
}

static esp_err_t status_get(httpd_req_t *req) {
    // /api/status is the browser heartbeat. A recent request keeps Talon USB
    // attached even when no BLE controller is connected.
    usb_owner_web_activity();
    char ip[16] = "";
    int rssi = 0;
    wifi_net_up(ip, &rssi);
    uint16_t rl, rr;
    talon_get_rumble(&rl, &rr);
    uint8_t rep[20];
    talon_report_build(rep);

    static const char *mode_names[] = { "sta", "setup", "wps" };
    static const char *wps_names[]  = { "idle", "connecting", "connected", "failed" };
    char btbuf[1024];
    bt_host_status_json(btbuf, sizeof(btbuf));
    char body[1536];
    snprintf(body, sizeof(body),
        "{\"mounted\":%d,\"ip\":\"%s\",\"rssi\":%d,"
        "\"mode\":\"%s\",\"ssid\":\"%s\",\"hostname\":\"" TALON_HOSTNAME "\","
        "\"wps\":\"%s\",\"wps_remaining\":%d,"
        "\"in_ok\":%lu,\"in_err\":%lu,\"rumble_pkts\":%lu,"
        "\"open\":%lu,\"reset\":%lu,\"ctrl_xid\":%lu,"
        "\"rumble_l\":%u,\"rumble_r\":%u,"
        "\"digital\":%u,\"free_heap\":%u,"
        "\"usb_active\":%d,\"usb_ble\":%d,\"usb_web\":%d,%s}",
        usb_owner_mounted() ? 1 : 0, ip, rssi,
        mode_names[wifi_net_mode()], wifi_net_ssid(),
        wps_names[wifi_net_wps_state()], wifi_net_wps_remaining(),
        (unsigned long)g_xid_in_ok, (unsigned long)g_xid_in_err,
        (unsigned long)g_xid_out_pkts,
        (unsigned long)g_xid_open, (unsigned long)g_xid_reset,
        (unsigned long)g_xid_ctrl_xid,
        rl, rr, rep[2], (unsigned)esp_get_free_heap_size(),
        usb_owner_active() ? 1 : 0,
        usb_owner_ble_active() ? 1 : 0,
        usb_owner_web_active() ? 1 : 0,
        btbuf);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

void webui_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 24;   // the default (8) silently drops registrations
    cfg.stack_size = 8192;       // OTA + JSON handlers need more than the 4 KB default
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET, .handler = root_get },
        { .uri = "/setup",           .method = HTTP_GET, .handler = setup_get },
        { .uri = "/ota",             .method = HTTP_GET, .handler = ota_page_get },
        { .uri = "/api/btn",         .method = HTTP_GET, .handler = btn_get },
        { .uri = "/api/axis",        .method = HTTP_GET, .handler = axis_get },
        { .uri = "/api/press",       .method = HTTP_GET, .handler = press_get },
        { .uri = "/api/release",     .method = HTTP_GET, .handler = release_get },
        { .uri = "/api/state",       .method = HTTP_GET, .handler = state_get },
        { .uri = "/api/status",      .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/igr",         .method = HTTP_GET, .handler = igr_get },
        { .uri = "/api/scan",        .method = HTTP_GET, .handler = scan_get },
        { .uri = "/api/wifi/save",   .method = HTTP_GET, .handler = wifi_save_get },
        { .uri = "/api/wifi/forget", .method = HTTP_GET, .handler = wifi_forget_get },
        { .uri = "/api/wps/start",   .method = HTTP_GET, .handler = wps_start_get },
        { .uri = "/api/bt/scan",     .method = HTTP_GET, .handler = bt_scan_get },
        { .uri = "/api/bt/connect",  .method = HTTP_GET, .handler = bt_connect_get },
        { .uri = "/api/bt/forget",   .method = HTTP_GET, .handler = bt_forget_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(srv, &uris[i]);
    ota_register(srv);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, err_404);
    ESP_LOGI(TAG, "web UI up on port %d", cfg.server_port);
}
