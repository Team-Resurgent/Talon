// Talon — BLE HID controller host. Scans for, pairs, and reads BLE HID
// gamepads and drives the Xbox input from them. BLE only (ESP32-S3 has no
// Bluetooth Classic), so DualShock/DualSense/Switch Pro cannot pair here.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the BLE stack + HID host and, if a controller is bonded (NVS),
// start trying to reconnect to it. Call after WiFi is up.
void bt_host_start(void);

// Blocking ~3 s scan; writes JSON [{"addr":"..","name":"..","rssi":-N,"gamepad":0|1},..].
int bt_host_scan_json(char *out, size_t cap);

// Pair/connect to a scanned address ("aa:bb:cc:dd:ee:ff"); bonds it in NVS.
bool bt_host_connect(const char *addr_str);

// Disconnect and forget the bonded controller.
void bt_host_forget(void);

// Status JSON fragment (no braces): connection/bond diagnostics, address/name, reports, raw report, trigger telemetry.
void bt_host_status_json(char *out, size_t cap);

// Disable the BT controller/stack. Called before an OTA flash write: BT
// coexistence changes flash synchronization in a way that asserts mid-write.
// Not meant to be resumed — the device reboots after OTA.
void bt_host_stop(void);

#ifdef __cplusplus
}
#endif
