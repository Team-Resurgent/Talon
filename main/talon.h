// Talon — shared state between the XID USB class driver and the web UI.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build the current 20-byte XID input report (thread-safe snapshot).
void talon_report_build(uint8_t out[20]);

// Set one control by name. Digital: up down left right start back ls rs
// (v: 0/1). Analog buttons: a b x y black white lt rt (v: 0..255; 1 means 255
// so digital-style callers work). Axes: lx ly rx ry (v: -32768..32767).
// Returns false for an unknown name.
bool talon_set_control(const char *name, int v);

// Release everything (all buttons up, axes centered).
void talon_reset_controls(void);

// Set the whole state atomically (gamepad forwarding). vals[13]:
// digital,a,b,x,y,black,white,lt,rt,lx,ly,rx,ry — analog 0..255, axes s16.
void talon_set_state_all(const int vals[13]);

// Last rumble values received from the Xbox (left/right actuator, 0..65535).
void talon_get_rumble(uint16_t *left, uint16_t *right);

// USB ownership arbitration (native USB D-/D+ are physically shared with the Xbox).
void xid_usb_set_enabled(bool enabled);

// Diagnostics for the heartbeat / status endpoint.
extern volatile uint32_t g_xid_in_ok;      // input reports delivered
extern volatile uint32_t g_xid_in_err;     // interrupt IN completions with error
extern volatile uint32_t g_xid_out_pkts;   // rumble packets received (EP or EP0)
extern volatile uint32_t g_xid_open;       // times the host configured iface0
extern volatile uint32_t g_xid_reset;      // bus resets seen
extern volatile uint32_t g_xid_ctrl_xid;   // XID/vendor EP0 requests answered

#ifdef __cplusplus
}
#endif
