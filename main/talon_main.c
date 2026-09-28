// Talon — ESP32-S3 WiFi-enabled original Xbox "Duke" controller.
//
// The native USB-OTG port plugs into the Xbox controller port and enumerates as
// a real Duke (XID device, 045E:0202). WiFi joins the LAN and serves a web UI /
// HTTP API that drives the input report — a controller you press from a browser.
//
// Console/flash is on UART0 (COM3). USB comes up before WiFi: the sibling
// Falcon project showed radio bring-up can disturb Xbox USB enumeration.
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "sdkconfig.h"
#include "xid_descriptors.h"
#include "talon.h"
#include "wifi_net.h"
#include "bt_host.h"
#include "led_status.h"
#include "usb_owner.h"

static const char *TAG = "talon";

void webui_start(void);
void ota_mark_valid(void);

#define TALON_BOOT_SLEEP_MS (3UL * 60UL * 1000UL)

// If Talon has not been claimed by either BLE or the web UI during the first
// three minutes after boot, shut the radios down and enter deep sleep. GPIO0
// is configured as a wake source so the physical BOOT button can wake Talon
// and start a fresh boot cycle.
static void boot_idle_sleep_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(TALON_BOOT_SLEEP_MS));

    bool ble = usb_owner_ble_active();
    bool web = usb_owner_web_active();
    if (ble || web) {
        ESP_LOGI(TAG, "3-minute boot check: keeping Talon awake (BLE=%d web=%d)",
                 ble ? 1 : 0, web ? 1 : 0);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "3-minute boot check: no BLE/web owner — entering deep sleep");

    // Stop USB first so GPIO19/20 are electrically released from the shared
    // Xbox controller bus.
    usb_owner_set_ble(false);
    // Give the ownership task time to uninstall TinyUSB and float GPIO19/20.
    // This is important because those pins are physically shared with the
    // Xbox controller port.
    for (int i = 0; i < 20 && usb_owner_active(); i++)
        vTaskDelay(pdMS_TO_TICKS(25));

    // Tear down Bluetooth completely before sleep.
    bt_host_stop();

    // Stop WiFi. Deep sleep will power down the radio, but stopping it first
    // avoids leaving an active connection/event stream during the transition.
    esp_wifi_disconnect();
    esp_wifi_stop();

    // Wake by pressing the physical BOOT button (GPIO0 -> LOW).
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);

    ESP_LOGI(TAG, "Talon sleeping — press BOOT or power-cycle to wake");
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();

    vTaskDelete(NULL);
}

// Physical BOOT button (GPIO0), matching Kratos so WiFi setup needs no phone:
//   * short press   -> WiFi setup (SoftAP "Talon-Setup"); status LED breathes white.
//   * hold (>= 3 s) -> WPS: then press the router's WPS button; LED blinks white.
static void boot_button_task(void *arg) {
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held = 0;
    bool long_fired = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            held++;
            if (held == 30 && !long_fired) {         // 3 s hold -> WPS
                ESP_LOGI(TAG, "BOOT held 3s — starting WPS");
                wifi_net_start_wps();
                long_fired = true;
            }
        } else {
            if (held >= 1 && !long_fired) {          // debounced short press -> AP setup
                ESP_LOGI(TAG, "BOOT short press — entering WiFi setup (SoftAP)");
                wifi_net_enter_setup();
            }
            held = 0;
            long_fired = false;
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "boot: reset_reason=%d", (int)esp_reset_reason());
    ESP_LOGI(TAG, "Talon: Xbox Duke controller (XID) emulator starting");

    // esp_tinyusb builds its tud_descriptor_*_cb from the pointers passed here;
    // the XID class driver registers itself via usbd_app_driver_get_cb.
    const tusb_desc_device_t *dev; const uint8_t *cfg; const char **strs; int nstr;
    xid_get_descriptors(&dev, &cfg, &strs, &nstr);
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = dev,
        .string_descriptor        = strs,
        .string_descriptor_count  = nstr,
        .configuration_descriptor = cfg,
        .external_phy             = false,
    };
    ESP_LOGI(TAG, "USB device configuration ready — starting USB ownership arbitration");
    usb_owner_start(&tusb_cfg);
    ESP_LOGI(TAG, "USB device starts detached until BLE or web activity needs it");

    // USB starts detached by design, so tud_mounted() must not be used as a
    // startup gate: the web UI/BLE may be what requests USB ownership later.
    // Bring WiFi and the web UI up immediately so they can acquire the USB bus.
    ESP_LOGI(TAG, "USB ownership is demand-driven — starting WiFi");
    led_status_start();
    wifi_net_start();
    webui_start();
    // BLE HID host comes up after WiFi so the coexistence arbiter is already
    // arbitrating the shared 2.4 GHz radio. BLE controllers can then be paired
    // from the web UI as an alternative to the browser gamepad relay.
    bt_host_start();
    xTaskCreate(boot_button_task, "bootbtn", 2560, NULL, tskIDLE_PRIORITY + 2, NULL);
    xTaskCreate(boot_idle_sleep_task, "boot_sleep", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
    // Everything came up — confirm this image so the OTA bootloader keeps it
    // (a firmware that crashes before here rolls back to the previous slot).
    ota_mark_valid();

    // Periodic UART status so a serial monitor sees the whole story even if
    // single event lines are missed — and so a reboot is obvious (t resets).
    for (uint32_t t = 0;; t++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        char ip[16] = "-"; int rssi = 0;
        wifi_net_up(ip, &rssi);
        uint16_t rl, rr; talon_get_rumble(&rl, &rr);
        static const char *modes[] = { "sta", "setup", "wps" };
        ESP_LOGI(TAG, "HB t=%lus mnt=%d net=%s ip=%s rssi=%d rst=%lu open=%lu xid=%lu "
                      "in_ok=%lu in_err=%lu rumble=%lu(%u/%u) free=%u",
                 (unsigned long)(t * 5), usb_owner_mounted() ? 1 : 0,
                 modes[wifi_net_mode()], ip, rssi,
                 (unsigned long)g_xid_reset, (unsigned long)g_xid_open,
                 (unsigned long)g_xid_ctrl_xid, (unsigned long)g_xid_in_ok,
                 (unsigned long)g_xid_in_err, (unsigned long)g_xid_out_pkts,
                 rl, rr, (unsigned)esp_get_free_heap_size());
    }
}
