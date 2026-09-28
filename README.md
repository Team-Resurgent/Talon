<h1 align="center">Talon</h1>

This is a fix of Team-Resurgents Talon.
I'm not sure if I can submit anything on the main repo so I made a small fork but all credit goes to EqUiNoX
What changed ?
First of all Talon is now pairing with a BLE controller (big success)
Secondly I swapped X-B buttons in the webapp to be similar to an Xbox controller
Also got the webapp to shown the keys pressed on the controller (not very usefull but it's there if someone need to test their controller)
Then added a 3 minutes delay to pair a device. If nothing if interacting with Talon it will send the ESP32 to sleep 

STEALTH MODE
It is now stealth on USB if nothing is paired. 
This is for people who want to have Talon "inside" the XBOX (there is just enough space behind port3-4 for an ESP32S3 to fit between the case and the shielding) 
and soldering GPIo19 to D- and GPIo20 to D+ on the controller motherboard (obviously 5V and GND need to be soldered too)
So now We can hook a real controller in port 1 and play wired or unplug it and use a wireless BLE controller with nothing hanging out of the XBOX.

I'm not a coder. I did some tinkering going back and forth during a whole day with ChatGPT. 
As far as I tested it's working fine and keeps the connection alive which isn't the case of OGXmini on a picoW.

<p align="center"><b>A WiFi &amp; Bluetooth-enabled original Xbox controller, emulated on an ESP32-S3</b></p>

<p align="center">
  <a href="https://github.com/Team-Resurgent/Talon/blob/main/LICENSE.md"><img src="https://img.shields.io/badge/License-GPLv3-blue.svg" alt="License: GPL v3"></a>
  <a href="https://github.com/Team-Resurgent/Talon/actions/workflows/release.yml"><img src="https://github.com/Team-Resurgent/Talon/actions/workflows/release.yml/badge.svg" alt="Release"></a>
  <a href="https://discord.gg/VcdSfajQGK"><img src="https://img.shields.io/badge/chat-on%20discord-7289da.svg?logo=discord" alt="Discord"></a>
</p>

<p align="center">
  <a href="https://ko-fi.com/J3J7L5UMN"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="ko-fi"></a>
  <a href="https://www.patreon.com/teamresurgent"><img src="https://img.shields.io/badge/Patreon-F96854?style=for-the-badge&logo=patreon&logoColor=white" alt="Patreon"></a>
</p>

<p align="center">
  <a href="https://github.com/Team-Resurgent/Talon/releases/latest"><img src="https://img.shields.io/badge/download-latest-brightgreen.svg?style=for-the-badge&logo=github" alt="Download"></a>
</p>

Talon makes an ESP32-S3 present itself as an **original Xbox "Duke" controller**
(Microsoft `045E:0202`, XID class `0x58/0x42`) on the Xbox controller port,
while joining your WiFi and serving a phone-friendly **web UI** — a controller
you press from a browser. It can also relay a physical controller (browser
Gamepad API or a directly-paired BLE pad) and fire the Cerbios In-Game-Reset
combos. Sibling project of Falcon, the Xbox Video Camera emulator, and built on
the same skeleton (ESP-IDF + esp_tinyusb + a custom application class driver).

<p align="center">
  <img src="docs/controller.png" alt="Controller web UI" width="32%">
  <img src="docs/setup.png" alt="WiFi setup" width="32%">
  <img src="docs/ota.png" alt="OTA firmware update" width="32%">
</p>
<p align="center"><sub>The browser controller (talon.local) · WiFi setup portal · OTA firmware update</sub></p>

## Hardware

Developed and tested on the **[Lonely Binary ESP32-S3 N16R8 Gold Edition](https://www.amazon.ca/dp/B0FFLXM9KL)**
(ESP32-S3, 16 MB flash, 8 MB PSRAM, **dual USB-C**, IPEX external antenna, onboard
WS2812 RGB LED on **GPIO48**).

Other **ESP32-S3** boards should work — just adjust `TALON_LED_GPIO` in
`main/led_status.h` if the RGB LED is on a different pin (or no LED). Other ESP32
variants **may** work but are **currently untested**:

- **ESP32-S2** — has native USB (the controller emulation would work) but **no
  Bluetooth at all**, so direct BLE controller pairing is unavailable.
- **ESP32 (original), C3, C6, H2** — **not supported**: they have no native
  USB-OTG device peripheral, which Talon needs to present itself as the pad.

If you get Talon running on another board, a report (or PR noting the pin
changes) is welcome.

<p align="center">
  <a href="https://www.amazon.ca/dp/B0FFLXM9KL"><img src="docs/board.jpg" alt="Lonely Binary ESP32-S3 N16R8 Gold Edition" width="55%"></a>
</p>

Wiring to the Xbox:

- **Native USB-C → the Xbox controller port.** This is the USB device the Xbox
  enumerates as the Duke, and it also powers Talon from the console's 5 V.
- **UART USB-C → your PC** for flashing and the serial console (COM3 here).

## How it works

The Duke is an XID device: one interface (class `0x58`, subclass `0x42`) with two
32-byte interrupt endpoints polled every 4 ms — EP2 IN carries the 20-byte input
report, EP2 OUT the 6-byte rumble report. On EP0 the Xbox reads a vendor **XID
descriptor** (`bmRequestType 0xC1, GET_DESCRIPTOR, wValue 0x4200`) and
**capabilities** (`bRequest 0x01`), and uses HID-style `GET_REPORT`/`SET_REPORT`.

- **`xid_descriptors.c`** — byte-faithful device/config descriptors (no strings,
  EP0 size 8, exactly like the real pad) + XID descriptor/capability blobs.
- **`xid_class.c`** — custom TinyUSB class driver: claims the XID interface,
  keeps an input report armed on EP2 IN at all times (refreshed per completion),
  receives rumble on EP2 OUT and EP0.
- **`talon_input.c`** — the shared controller state (spinlock-guarded).
- **`wifi_net.c` / `webui.c` / `index.html`** — WiFi station + HTTP API + web UI.

## Finding it

Talon advertises **mDNS**, so once it's on your network it's at
**http://talon.local/** (and appears as host `talon` in your router's device
list). The web page has a ⚙ link to the WiFi setup page.

## WiFi setup (like Kratos)

Credentials live in NVS. With none stored — or if the stored ones fail to
connect within 45 s — Talon starts an open SoftAP with a captive portal:

1. Join the WiFi network **`Talon-Setup`** (open, no password).
2. If the setup page doesn't pop up automatically, browse to
   **`http://192.168.4.1`**.
3. **Scan + join** your network (or use **WPS**).

Entered/negotiated credentials are saved and rejoined automatically on boot.
For a personal build you can also compile credentials in (`main/wifi_creds.h`,
seeded into NVS on first boot).

The **BOOT button** provides phone-free setup, matching Kratos:

| Gesture | Action | Status LED (WS2812, GPIO48) |
|---|---|---|
| Short press | Open the `Talon-Setup` portal | breathing white |
| Hold ~3 s | Start WPS (then press the router's WPS) | blinking white |

## Physical controller passthrough

Two ways to drive the Xbox with a real controller:

- **Browser Gamepad API** — plug/pair a controller into the phone/PC viewing the
  page; its state streams to `/api/state` at ~30 Hz. Works with any controller
  the browsing device supports (including DualSense over the PC's Bluetooth).
- **Direct BLE pairing** — pair a **BLE** controller straight to the ESP32-S3
  from the *Bluetooth Controller* panel on the controller page (no browser
  needed once bonded).

### Bluetooth controller support

Direct pairing is **BLE-only** — the ESP32-S3 has no Bluetooth Classic radio.
A generic HID parser maps standard gamepad reports, so most BLE HID pads should
work, but the button/axis mapping is still **experimental** and may need a
per-controller tweak (the raw report is exposed at `/api/status` as `bt_last`
to help dial it in).

| Controller | Direct BLE pairing | Notes |
|---|---|---|
| Xbox Wireless Controller (Model 1708+, Series, Elite 2) | ✅ expected | These use BLE HID |
| 8BitDo pads in a BLE/"D-input" mode | ✅ expected | Mode switch varies per model |
| Generic BLE HID gamepads | ✅ expected | Standard HID usages |
| Sony DualShock 4 / DualSense | ❌ no | Bluetooth **Classic** — use the browser relay |
| Nintendo Switch Pro / Joy-Con | ❌ no | Bluetooth **Classic** — use the browser relay |
| Xbox 360 / original wireless | ❌ no | Proprietary 2.4 GHz, not Bluetooth |

Anything not directly pairable still works through the **browser Gamepad API**
relay above (pair it to the phone/PC, not to Talon). *Confirmed-working BLE pads
will be listed here as they're tested — reports welcome.*

## OTA updates

Talon runs from dual OTA slots. Build a new firmware and upload `talon.bin` to
`http://talon.local/ota` (drag-and-drop page) — it flashes the inactive slot,
verifies, and reboots into it. If the new image fails, the bootloader rolls back
to the previous slot.

## Cerbios IGR

If the console runs the **Cerbios** BIOS, Talon exposes its default In-Game
Reset combos as one-tap shortcuts (collapsible section on the controller page,
or `/api/igr?a=...`): `dash` (LT+RT+Start+Back), `game`, `full`, `cycle`,
`shutdown`, `screen`. Combos and their nibble encoding come from
CerbiosToolInternal (`Config.cs`).

## Web API

Everything is `GET`, so `curl` can drive the pad:

```
/api/press?b=down&ms=120       tap a control (press, hold ms, release)
/api/btn?b=a&v=1               hold (v=1) / release (v=0)
/api/axis?a=lx&v=-32768        analog: lt/rt 0..255, lx/ly/rx/ry -32768..32767
/api/state?v=d,a,b,x,y,bl,wh,lt,rt,lx,ly,rx,ry   whole report at once
/api/release                   release everything
/api/igr?a=dash                Cerbios IGR shortcut
/api/status                    JSON diagnostics (mode, ssid, wps, USB, rumble)
/api/scan                      visible networks (JSON)
/api/wifi/save?ssid=&pass=     save credentials + join
/api/wifi/forget               erase credentials, back to setup AP
/api/wps/start                 WPS push-button join
```

Controls: `up down left right start back ls rs a b x y black white lt rt lx ly rx ry`.

## Build & flash

ESP-IDF ≥ 5.5, ESP32-S3. The **native USB port is the controller**, so
flash/log over the **UART bridge (COM3)**:

```bash
cp main/wifi_creds.h.example main/wifi_creds.h   # fill in SSID/password
idf.py set-target esp32s3
idf.py -p COM3 build flash monitor
```

USB comes up first and WiFi only after enumeration (or an 8 s timeout) —
Falcon showed radio bring-up can disturb the Xbox's timing-strict enumeration.
The dwc2 controller runs in **slave mode** (`CONFIG_TINYUSB_MODE_SLAVE=y`);
the S3's OTG DMA engine corrupts EP0 SETUP under load (proven on Falcon).
