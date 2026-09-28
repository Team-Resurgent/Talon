// Talon — USB ownership arbitration.
//
// USB D-/D+ are physically shared with the Xbox's wired controller port. The
// Talon USB device therefore must be electrically/logically detached whenever
// neither a BLE controller nor an active web client needs it.
#include "usb_owner.h"
#include "talon.h"
#include "tinyusb.h"
#include "tusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "talon.usb";

// The web UI polls /api/status every ~2 seconds. Five seconds gives a useful
// margin for one or two delayed HTTP polls while still releasing the bus soon
// after a browser closes/crashes or loses its network connection.
#define WEB_LEASE_MS 5000
#define USB_OWNER_POLL_MS 100

static volatile bool s_ble_active;
static volatile TickType_t s_web_last_activity;
static volatile bool s_usb_active;
static TaskHandle_t s_task;
static tinyusb_config_t s_tusb_cfg;


static void release_usb_pins(void)
{
    // GPIO19/20 are the ESP32-S3 native USB D-/D+ pins. After TinyUSB is
    // uninstalled, explicitly return them to ordinary floating inputs so the
    // physically shared Xbox USB bus is not biased or driven by the ESP32.
    gpio_reset_pin(GPIO_NUM_19);
    gpio_reset_pin(GPIO_NUM_20);
    gpio_set_direction(GPIO_NUM_19, GPIO_MODE_INPUT);
    gpio_set_direction(GPIO_NUM_20, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_19, GPIO_FLOATING);
    gpio_set_pull_mode(GPIO_NUM_20, GPIO_FLOATING);
    ESP_LOGI(TAG, "USB pins RELEASED: GPIO19=D- GPIO20=D+ INPUT/FLOATING");
}

static bool web_is_active(TickType_t now)
{
    TickType_t last = s_web_last_activity;
    if (last == 0) return false;
    return (now - last) < pdMS_TO_TICKS(WEB_LEASE_MS);
}

static bool desired_active(TickType_t now)
{
    return s_ble_active || web_is_active(now);
}

static void apply_usb_state(bool active)
{
    if (active == s_usb_active) return;

    if (active) {
        ESP_LOGI(TAG, "USB ownership ACTIVE (BLE=%d web=%d) — installing USB device",
                 s_ble_active ? 1 : 0,
                 web_is_active(xTaskGetTickCount()) ? 1 : 0);

        // Re-create the complete ESP-IDF TinyUSB device stack. On the ESP32-S3
        // this is important because tud_disconnect() only removes the USB
        // pull-up; it does not release the native USB peripheral/PHY from GPIO19/20.
        esp_err_t err = tinyusb_driver_install(&s_tusb_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
            return;
        }

        xid_usb_set_enabled(true);
        s_usb_active = true;
    } else {
        ESP_LOGI(TAG, "USB ownership RELEASED (BLE=0 web=0) — uninstalling USB device");

        // Stop XID endpoint/SOF activity before tearing down the TinyUSB stack.
        xid_usb_set_enabled(false);

        esp_err_t err = tinyusb_driver_uninstall();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb_driver_uninstall failed: %s", esp_err_to_name(err));
            // Keep the logical state active if the stack could not be torn down.
            xid_usb_set_enabled(true);
            return;
        }

        s_usb_active = false;
        release_usb_pins();
    }
}

static void usb_owner_task(void *arg)
{
    (void)arg;

    // USB is deliberately NOT installed while there is no owner. This fully
    // releases the ESP32-S3 USB peripheral/PHY and therefore GPIO19/20.
    s_usb_active = false;
    xid_usb_set_enabled(false);
    release_usb_pins();
    ESP_LOGI(TAG, "USB ownership initial state: RELEASED");

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        apply_usb_state(desired_active(now));
        vTaskDelay(pdMS_TO_TICKS(USB_OWNER_POLL_MS));
    }
}

void usb_owner_start(const void *tusb_config)
{
    if (s_task) return;
    s_ble_active = false;
    s_web_last_activity = 0;
    s_usb_active = false;

    // tinyusb_config_t is intentionally copied here because app_main() may
    // return from the setup scope after starting the owner task.
    s_tusb_cfg = *(const tinyusb_config_t *)tusb_config;

    BaseType_t rc = xTaskCreate(usb_owner_task, "usb_owner", 4096, NULL,
                                tskIDLE_PRIORITY + 3, &s_task);
    if (rc != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "failed to create USB ownership task");
    }
}

void usb_owner_set_ble(bool connected)
{
    s_ble_active = connected;
}

void usb_owner_web_activity(void)
{
    s_web_last_activity = xTaskGetTickCount();
}

bool usb_owner_active(void)
{
    return s_usb_active;
}

bool usb_owner_ble_active(void)
{
    return s_ble_active;
}

bool usb_owner_web_active(void)
{
    return web_is_active(xTaskGetTickCount());
}


bool usb_owner_mounted(void)
{
    if (!s_usb_active) return false;
    return tud_mounted();
}
