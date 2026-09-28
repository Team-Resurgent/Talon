// Talon — BLE HID controller host (see bt_host.h). Uses ESP-IDF's esp_hidh
// (Bluedroid BLE) plus the vendored esp_hid_gap scan helper. Discovered/paired
// controllers stream input reports; a generic HID parser (bt_hidmap) maps them
// onto the XID input state, so the Xbox sees the same pad the browser relay
// drives.
#include <string.h>
#include <stdio.h>
#include "bt_host.h"
#include "bt_hidmap.h"
#include "talon.h"
#include "usb_owner.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_hidh.h"
#include "esp_hid_common.h"
#include "esp_hid_gap.h"
#include "esp_bt.h"
#include "esp_bt_main.h"

static const char *TAG = "talon.bt";
#define NVS_NS "talon"

static esp_hidh_dev_t *s_dev;
static volatile bool s_connected;
static bool     s_have_bond;
static uint8_t  s_bond_addr[6];
static uint8_t  s_bond_atype;
static char     s_dev_name[40];
static hid_layout_t s_layout;
static bool     s_layout_ok;
static volatile uint32_t s_reports;
static uint8_t  s_last_raw[32];
static uint8_t  s_last_len, s_last_id;

// Xbox Series BLE rumble is HID Output Report ID 3. The report has an
// actuator-enable nibble followed by four 0..100 magnitude bytes, then
// duration, pause and loop count. We use the first two motors for the
// Xbox XID left/right rumble channels and leave trigger motors off.
static TaskHandle_t s_rumble_task;

static uint8_t rumble_percent(uint16_t v) {
    return (uint8_t)(((uint32_t)v * 100u + 32767u) / 65535u);
}

static void send_rumble(uint16_t left, uint16_t right) {
    if (!s_dev || !s_connected || !esp_hidh_dev_exists(s_dev)) return;

    uint8_t l = rumble_percent(left);
    uint8_t r = rumble_percent(right);

    /*
     * Xbox Series X/S BLE rumble Report ID 3:
     *
     * byte 0: motor enable bits
     *   bit 0 = center
     *   bit 1 = shake
     *   bit 2 = right/main
     *   bit 3 = left/main
     *
     * bytes 1-4: motor magnitudes
     *   0 = center
     *   1 = shake
     *   2 = right/main
     *   3 = left/main
     *
     * byte 5 = duration
     * byte 6 = pause
     * byte 7 = loop count
     */

    uint8_t report[8] = {
        (uint8_t)((l || r) ? 0x03 : 0x00),  // right + left motors
        0x00,                                // center
        0x00,                                // shake
        r,                                   // right/main
        l,                                   // left/main
        (uint8_t)((l || r) ? 10 : 0),      // duration
        0x00,                                // pause
        0x00                                 // loop
    };

    esp_err_t err = esp_hidh_dev_output_set(
        s_dev,
        0,
        3,
        report,
        sizeof(report)
    );

    ESP_LOGI(TAG,
             "BLE RUMBLE: L=%u R=%u report=%02X %02X %02X %02X %02X %02X %02X %02X err=%s",
             l, r,
             report[0], report[1], report[2], report[3],
             report[4], report[5], report[6], report[7],
             esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BLE rumble output failed: %s", esp_err_to_name(err));
    }
}

static void rumble_task(void *arg) {
    uint16_t last_left = 0;
    uint16_t last_right = 0;
    TickType_t last_send = 0;

    for (;;) {
        uint16_t left = 0;
        uint16_t right = 0;

        talon_get_rumble(&left, &right);

        TickType_t now = xTaskGetTickCount();

        /*
         * Send immediately when the rumble value changes.
         * Also refresh an active rumble every 50 ms so a short
         * duration does not allow the controller to stop vibrating
         * while the Xbox continues requesting the same value.
         */
        bool changed = (left != last_left || right != last_right);
        bool refresh = ((left || right) &&
                        ((now - last_send) >= pdMS_TO_TICKS(50)));

        if (changed || refresh) {
            send_rumble(left, right);

            last_left = left;
            last_right = right;
            last_send = now;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---- bond persistence ------------------------------------------------------

static void bond_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_bond_addr);
    if (nvs_get_blob(h, "bt_addr", s_bond_addr, &n) == ESP_OK && n == 6) {
        uint8_t at = 0;
        nvs_get_u8(h, "bt_atype", &at);
        s_bond_atype = at;
        s_have_bond = true;
    }
    nvs_close(h);
}

static void bond_store(const uint8_t addr[6], uint8_t atype) {
    memcpy(s_bond_addr, addr, 6);
    s_bond_atype = atype;
    s_have_bond = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "bt_addr", addr, 6);
        nvs_set_u8(h, "bt_atype", atype);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void bond_erase(void) {
    s_have_bond = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "bt_addr");
        nvs_erase_key(h, "bt_atype");
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---- connect / reconnect ---------------------------------------------------

static void open_bond_task(void *arg) {
    (void)arg;
    // Retry the bonded controller until it answers (it may be asleep/off).
    while (s_have_bond && !s_connected) {
        ESP_LOGI(TAG, "connecting to bonded controller %02x:%02x:%02x:%02x:%02x:%02x",
                 s_bond_addr[0], s_bond_addr[1], s_bond_addr[2],
                 s_bond_addr[3], s_bond_addr[4], s_bond_addr[5]);
        esp_hidh_dev_open(s_bond_addr, ESP_HID_TRANSPORT_BLE, s_bond_atype);
        // OPEN/CLOSE arrive on the event handler; wait before retrying.
        for (int i = 0; i < 50 && !s_connected && s_have_bond; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_connected) vTaskDelay(pdMS_TO_TICKS(3000));
    }
    vTaskDelete(NULL);
}

static void start_reconnect(void) {
    if (s_have_bond && !s_connected)
        xTaskCreate(open_bond_task, "bt_open", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
}

// ---- HID events ------------------------------------------------------------

static void build_layout(esp_hidh_dev_t *dev) {
    s_layout_ok = false;
    size_t nmaps = 0;
    esp_hid_raw_report_map_t *maps = NULL;
    if (esp_hidh_dev_report_maps_get(dev, &nmaps, &maps) != ESP_OK || !maps || !nmaps)
        return;
    // Parse the first map that yields gamepad fields.
    for (size_t m = 0; m < nmaps; m++) {
        if (maps[m].data && maps[m].len &&
            hid_parse_descriptor(maps[m].data, maps[m].len, &s_layout)) {
            s_layout_ok = true;
            ESP_LOGI(TAG, "parsed HID map %u: %d fields", (unsigned)m, s_layout.n);
            return;
        }
    }
    ESP_LOGW(TAG, "no usable gamepad fields in report map");
}

static void hidh_cb(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_hidh_event_t ev = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)data;

    switch (ev) {
    case ESP_HIDH_OPEN_EVENT: {
        if (!p->open.dev) break;
        s_dev = p->open.dev;
        const char *nm = esp_hidh_dev_name_get(s_dev);
        strlcpy(s_dev_name, nm ? nm : "BLE controller", sizeof(s_dev_name));
        const uint8_t *bda = esp_hidh_dev_bda_get(s_dev);
        if (bda) bond_store(bda, s_bond_atype);
        build_layout(s_dev);
        s_connected = true;
        usb_owner_set_ble(true);
        ESP_LOGI(TAG, "controller open: %s", s_dev_name);
        break;
    }
    case ESP_HIDH_INPUT_EVENT: {
        s_reports++;
        s_last_id  = (uint8_t)p->input.report_id;
        s_last_len = p->input.length > sizeof(s_last_raw) ? sizeof(s_last_raw) : (uint8_t)p->input.length;
        memcpy(s_last_raw, p->input.data, s_last_len);
        if (s_layout_ok) {
            // The BLE pad fully owns the input while connected: rebuild the
            // whole report from this frame (absolute state, not deltas).
            int st[13];
            memset(st, 0, sizeof(st));
            if (hid_report_to_state(&s_layout, (uint8_t)p->input.report_id,
                                    p->input.data, p->input.length, st))
                talon_set_state_all(st);
        }
        break;
    }
    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGW(TAG, "controller closed");
        s_connected = false;
        usb_owner_set_ble(false);
        s_layout_ok = false;
        if (p->close.dev) esp_hidh_dev_free(p->close.dev);
        s_dev = NULL;
        talon_reset_controls();
        start_reconnect();                       // auto-reconnect if still bonded
        break;
    default:
        break;
    }
}

// ---- public API ------------------------------------------------------------

void bt_host_start(void) {
    // esp_hid_gap_init brings up the BT controller + Bluedroid + GAP in BLE
    // mode; WiFi is already running, so the coexistence arbiter shares the radio.
    if (esp_hid_gap_init(HIDH_BLE_MODE) != ESP_OK) {
        ESP_LOGE(TAG, "BLE GAP init failed — BT controller support disabled");
        return;
    }
    esp_hidh_config_t cfg = {
        .callback = hidh_cb,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    if (esp_hidh_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init failed");
        return;
    }
    ESP_LOGI(TAG, "BLE HID host ready");
    bond_load();
    start_reconnect();
    if (!s_rumble_task) {
        xTaskCreate(rumble_task, "bt_rumble", 3072, NULL,
                    tskIDLE_PRIORITY + 1, &s_rumble_task);
    }
}

static void addr_to_str(const uint8_t a[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}
static bool str_to_addr(const char *s, uint8_t out[6]) {
    unsigned int v[6];
    char tail = 0;
    if (!s || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c",
                     &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6)
        return false;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) return false;
        out[i] = (uint8_t)v[i];
    }
    return true;
}

// Remember the addr->addr_type from the most recent scan, so connect() knows
// how to open a public vs random address.
static uint8_t s_scan_addr[8][6];
static uint8_t s_scan_atype[8];
static int     s_scan_n;

int bt_host_scan_json(char *out, size_t cap) {
    size_t num = 0;
    esp_hid_scan_result_t *results = NULL;
    esp_hid_scan(3, &num, &results);

    s_scan_n = 0;
    size_t o = 0;
    o += snprintf(out + o, cap - o, "[");
    int written = 0;
    for (esp_hid_scan_result_t *r = results; r && o + 96 < cap; r = r->next) {
        if (r->transport != ESP_HID_TRANSPORT_BLE) continue;
        char addr[18];
        addr_to_str(r->bda, addr);
        int is_pad = (r->usage == ESP_HID_USAGE_GAMEPAD || r->usage == ESP_HID_USAGE_JOYSTICK);
        char nm[40];
        strlcpy(nm, r->name ? r->name : "", sizeof(nm));
        for (char *c = nm; *c; c++) if (*c == '"' || *c == '\\') *c = ' ';
        o += snprintf(out + o, cap - o, "%s{\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"gamepad\":%d}",
                      written ? "," : "", addr, nm, r->rssi, is_pad);
        written++;
        if (s_scan_n < 8) { memcpy(s_scan_addr[s_scan_n], r->bda, 6);
                            s_scan_atype[s_scan_n] = r->ble.addr_type; s_scan_n++; }
    }
    snprintf(out + o, cap - o, "]");
    if (results) esp_hid_scan_results_free(results);
    return written;
}

bool bt_host_connect(const char *addr_str) {
    uint8_t addr[6];
    ESP_LOGI(TAG, "bt_host_connect('%s')", addr_str ? addr_str : "(null)");
    if (!str_to_addr(addr_str, addr)) {
        ESP_LOGE(TAG, "invalid Bluetooth address: '%s'", addr_str ? addr_str : "(null)");
        return false;
    }
    // Find the address type from the last scan (default public if unseen).
    uint8_t atype = 0;
    for (int i = 0; i < s_scan_n; i++)
        if (!memcmp(s_scan_addr[i], addr, 6)) { atype = s_scan_atype[i]; break; }
    s_bond_atype = atype;
    bond_store(addr, atype);
    start_reconnect();
    return true;
}

void bt_host_forget(void) {
    bool was = s_have_bond;
    bond_erase();
    if (s_dev && esp_hidh_dev_exists(s_dev)) esp_hidh_dev_close(s_dev);
    s_connected = false;
    usb_owner_set_ble(false);
    if (was) talon_reset_controls();
    ESP_LOGI(TAG, "controller forgotten");
}

void bt_host_stop(void) {
    s_have_bond = false;                 // stop the reconnect task's retry loop
    if (s_dev && esp_hidh_dev_exists(s_dev)) esp_hidh_dev_close(s_dev);
    s_connected = false;
    usb_owner_set_ble(false);
    esp_hidh_deinit();
    // Fully tear BT down (not just disable) so the WiFi/BT software-coexistence
    // layer is removed — with coex still registered, the first OTA flash write
    // asserts (xQueueSemaphoreTake, scheduler suspended). Bluedroid before the
    // controller; best-effort, the device reboots after OTA anyway.
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));      // let controller teardown settle
    ESP_LOGI(TAG, "BT torn down for OTA");
}

void bt_host_status_json(char *out, size_t cap)
{
    char addr[18] = "";
    if (s_have_bond)
        addr_to_str(s_bond_addr, addr);

    // Last raw report as hex, for empirical mapping.
    char hex[70];
    size_t ho = 0;

    for (int i = 0; i < s_last_len && ho + 3 < sizeof(hex); i++)
        ho += snprintf(hex + ho, sizeof(hex) - ho, "%02x",
                       s_last_raw[i]);

    hex[ho] = '\0';

    // Current decoded controller state.
    int state[13] = {0};
    hid_get_state(state);

    int digital = state[S_DIGITAL];

    snprintf(out, cap,
        "\"bt_connected\":%d,"
        "\"bt_bonded\":%d,"
        "\"bt_addr\":\"%s\","
        "\"bt_name\":\"%s\","
        "\"bt_reports\":%lu,"
        "\"bt_last_id\":%u,"
        "\"bt_last\":\"%s\","
        "\"bt_mapped\":%d,"

        "\"a\":%d,"
        "\"b\":%d,"
        "\"x\":%d,"
        "\"y\":%d,"
        "\"lb\":%d,"
        "\"rb\":%d,"
        "\"back\":%d,"
        "\"start\":%d,"
        "\"ls\":%d,"
        "\"rs\":%d,"
        "\"up\":%d,"
        "\"down\":%d,"
        "\"left\":%d,"
        "\"right\":%d,"

        "\"lt\":%d,"
        "\"rt\":%d,"
        "\"lx\":%d,"
        "\"ly\":%d,"
        "\"rx\":%d,"
        "\"ry\":%d",

        s_connected ? 1 : 0,
        s_have_bond ? 1 : 0,
        addr,
        s_dev_name,
        (unsigned long)s_reports,
        s_last_id,
        hex,
        s_layout_ok ? 1 : 0,

        state[S_A] > 0 ? 1 : 0,
        state[S_B] > 0 ? 1 : 0,
        state[S_X] > 0 ? 1 : 0,
        state[S_Y] > 0 ? 1 : 0,

        state[S_WHITE] > 0 ? 1 : 0,  // LB
        state[S_BLACK] > 0 ? 1 : 0,  // RB

        (digital & D_BACK)  ? 1 : 0,
        (digital & D_START) ? 1 : 0,
        (digital & D_LS)    ? 1 : 0,
        (digital & D_RS)    ? 1 : 0,

        (digital & D_UP)    ? 1 : 0,
        (digital & D_DOWN)  ? 1 : 0,
        (digital & D_LEFT)  ? 1 : 0,
        (digital & D_RIGHT) ? 1 : 0,

        state[S_LT],
        state[S_RT],
        state[S_LX],
        state[S_LY],
        state[S_RX],
        state[S_RY]);
}