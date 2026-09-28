// Talon — USB ownership arbitration.
// The ESP32-S3 native USB bus is physically shared with a wired Xbox controller.
// Keep Talon detached unless BLE or a recently-active web client needs it.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the ownership task. The task installs/uninstalls TinyUSB as ownership changes.
void usb_owner_start(const void *tusb_config);

// Report whether the BLE controller currently needs Talon's USB device.
void usb_owner_set_ble(bool connected);

// Refresh the web-client activity lease. Called by the HTTP heartbeat/API.
void usb_owner_web_activity(void);

// Diagnostics: current ownership and recent web activity.
bool usb_owner_active(void);
bool usb_owner_ble_active(void);
bool usb_owner_web_active(void);

// True only when Talon currently owns USB and the Xbox has enumerated it.
bool usb_owner_mounted(void);

#ifdef __cplusplus
}
#endif
