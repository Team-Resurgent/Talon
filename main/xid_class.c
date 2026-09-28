// Talon — custom TinyUSB application class driver for the XID (Xbox Input
// Device) gamepad interface.
//
// Owns interface 0 (class 0x58 / subclass 0x42) and its two interrupt
// endpoints. The Xbox polls EP2 IN every 4 ms for the 20-byte input report and
// writes 6-byte rumble reports on EP2 OUT (or via EP0 SET_REPORT). We keep one
// IN transfer armed at all times, refreshed with the latest state in the
// completion callback, so every host poll sees fresh data — exactly like a real
// pad. Modeled on falcon_class.c (the sibling camera emulator), minus all the
// isochronous pain.
#include <string.h>
#include "tusb.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "esp_log.h"
#include "xid_descriptors.h"
#include "talon.h"

static const char *TAG = "talon.xid";

// XID EP0 request numbers.
#define XID_REQ_GET_CAPABILITIES 0x01
#define XID_DESC_TYPE            0x42
#define HID_REQ_GET_REPORT       0x01
#define HID_REQ_SET_REPORT       0x09

static uint8_t s_rhport;
static uint8_t s_ep_in, s_ep_out;
static bool    s_configured;
static volatile bool s_usb_enabled = true;
static volatile bool s_ota_quiesce;   // set during OTA: stop all USB endpoint arming

volatile uint32_t g_xid_in_ok, g_xid_in_err, g_xid_out_pkts,
                  g_xid_open, g_xid_reset, g_xid_ctrl_xid;

static uint16_t s_rumble_l, s_rumble_r;

// Endpoint + EP0 staging buffers (must outlive the transfer they're handed to).
static uint8_t s_in_buf[XID_REPORT_LEN] CFG_TUSB_MEM_ALIGN;
static uint8_t s_out_buf[32]            CFG_TUSB_MEM_ALIGN;
static uint8_t s_ctrl_buf[32]           CFG_TUSB_MEM_ALIGN;

void talon_get_rumble(uint16_t *left, uint16_t *right) {
    if (left)  *left  = s_rumble_l;
    if (right) *right = s_rumble_r;
}

// Rumble report: 00 06 <left u16 LE> <right u16 LE>.
static void parse_rumble(const uint8_t *buf, uint32_t len) {
    if (len < XID_RUMBLE_LEN) return;
    s_rumble_l = (uint16_t)(buf[2] | (buf[3] << 8));
    s_rumble_r = (uint16_t)(buf[4] | (buf[5] << 8));
    g_xid_out_pkts++;
}

// Keep an input report armed on the interrupt IN endpoint. claim() failing just
// means a transfer is already in flight — fine, the completion will re-arm.
// Stop all USB endpoint activity ahead of an OTA flash write. The SOF callback
// runs inline in the dwc2 ISR and calls usbd_edpt_claim (a mutex); a flash erase
// disables the cache and suspends the scheduler, so a SOF landing in that window
// asserts (xQueueSemaphoreTake with scheduler suspended). Quiescing here makes
// every arm path early-return, so no mutex is taken from the ISR. The device
// reboots after OTA, so the Xbox link going idle is fine.
void xid_usb_set_enabled(bool enabled)
{
    s_usb_enabled = enabled;
    if (!enabled && s_configured)
        usbd_sof_enable(s_rhport, SOF_CONSUMER_USER, false);
}

void xid_ota_quiesce(void) {
    s_ota_quiesce = true;
    if (s_configured) usbd_sof_enable(s_rhport, SOF_CONSUMER_USER, false);
}

static void arm_in(uint8_t rhport) {
    if (!s_configured || !s_usb_enabled || s_ota_quiesce) return;
    if (!usbd_edpt_claim(rhport, s_ep_in)) return;
    talon_report_build(s_in_buf);
    if (!usbd_edpt_xfer(rhport, s_ep_in, s_in_buf, XID_REPORT_LEN, false)) {
        usbd_edpt_release(rhport, s_ep_in);
    }
}

static void arm_out(uint8_t rhport) {
    if (!s_configured || !s_usb_enabled || s_ota_quiesce) return;
    if (!usbd_edpt_claim(rhport, s_ep_out)) return;
    if (!usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf), false)) {
        usbd_edpt_release(rhport, s_ep_out);
    }
}

// ---- usbd_class_driver_t callbacks ----------------------------------------

static void xid_init(void) {
    s_configured = false;
}

static void xid_reset(uint8_t rhport) {
    (void)rhport;
    g_xid_reset++;
    s_configured = false;
    s_rumble_l = s_rumble_r = 0;
}

// Claim the XID interface and open both interrupt endpoints.
static uint16_t xid_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
    if (itf->bInterfaceClass != XID_ITF_CLASS ||
        itf->bInterfaceSubClass != XID_ITF_SUBCLASS) return 0;

    uint16_t const drv_len = (uint16_t)(sizeof(tusb_desc_interface_t) +
                                        itf->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
    TU_VERIFY(max_len >= drv_len, 0);

    uint8_t const *p = tu_desc_next(itf);
    for (int i = 0; i < itf->bNumEndpoints; i++) {
        tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
        TU_ASSERT(usbd_edpt_open(rhport, ep), 0);
        if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) s_ep_in = ep->bEndpointAddress;
        else                                                  s_ep_out = ep->bEndpointAddress;
        p = tu_desc_next(p);
    }

    s_rhport = rhport;
    s_configured = true;
    g_xid_open++;

    // Prime both pipes now; completions keep them armed from here on. SOF acts
    // as a watchdog in case a submit ever fails.
    arm_out(rhport);
    arm_in(rhport);
    usbd_sof_enable(rhport, SOF_CONSUMER_USER, true);
    ESP_LOGI(TAG, "XID interface opened (ep_in=%02x ep_out=%02x)", s_ep_in, s_ep_out);
    return drv_len;
}

static bool xid_control_xfer(uint8_t rhport, uint8_t stage,
                             tusb_control_request_t const *request) {
    switch (request->bmRequestType_bit.type) {

    case TUSB_REQ_TYPE_STANDARD:
        if (stage != CONTROL_STAGE_SETUP) return true;
        switch (request->bRequest) {
            case TUSB_REQ_GET_INTERFACE: {
                static uint8_t alt0 = 0;
                return tud_control_xfer(rhport, request, &alt0, 1);
            }
            case TUSB_REQ_SET_INTERFACE:
                return tud_control_status(rhport, request);
            default:
                return false;
        }

    case TUSB_REQ_TYPE_CLASS:
        // HID-style GET/SET_REPORT on EP0.
        if (request->bRequest == HID_REQ_GET_REPORT && request->wValue == 0x0100 &&
            request->bmRequestType_bit.direction == TUSB_DIR_IN) {
            if (stage != CONTROL_STAGE_SETUP) return true;
            talon_report_build(s_ctrl_buf);
            return tud_control_xfer(rhport, request, s_ctrl_buf,
                                    tu_min16(request->wLength, XID_REPORT_LEN));
        }
        if (request->bRequest == HID_REQ_SET_REPORT && request->wValue == 0x0200 &&
            request->bmRequestType_bit.direction == TUSB_DIR_OUT) {
            if (stage == CONTROL_STAGE_SETUP) {
                return tud_control_xfer(rhport, request, s_ctrl_buf,
                                        tu_min16(request->wLength, sizeof(s_ctrl_buf)));
            }
            if (stage == CONTROL_STAGE_ACK) parse_rumble(s_ctrl_buf, request->wLength);
            return true;
        }
        return false;

    default:
        return false;
    }
}

// XID descriptor + capability reads arrive as VENDOR-type EP0 requests
// (bmRequestType 0xC1). This TinyUSB routes ALL vendor control requests to this
// application hook before the per-interface class-driver dispatch, so they must
// be answered here — a STALL makes the Xbox's xpad driver silently give up and
// never poll the pad.
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if (request->bmRequestType_bit.direction != TUSB_DIR_IN) return false;
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bRequest == TUSB_REQ_GET_DESCRIPTOR &&
        request->wValue == (XID_DESC_TYPE << 8)) {
        g_xid_ctrl_xid++;
        memcpy(s_ctrl_buf, xid_desc_xid, sizeof(xid_desc_xid));
        return tud_control_xfer(rhport, request, s_ctrl_buf,
                                tu_min16(request->wLength, sizeof(xid_desc_xid)));
    }
    if (request->bRequest == XID_REQ_GET_CAPABILITIES && request->wValue == 0x0100) {
        g_xid_ctrl_xid++;
        memcpy(s_ctrl_buf, xid_caps_in, sizeof(xid_caps_in));
        return tud_control_xfer(rhport, request, s_ctrl_buf,
                                tu_min16(request->wLength, sizeof(xid_caps_in)));
    }
    if (request->bRequest == XID_REQ_GET_CAPABILITIES && request->wValue == 0x0200) {
        g_xid_ctrl_xid++;
        memcpy(s_ctrl_buf, xid_caps_out, sizeof(xid_caps_out));
        return tud_control_xfer(rhport, request, s_ctrl_buf,
                                tu_min16(request->wLength, sizeof(xid_caps_out)));
    }
    return false;
}

static bool xid_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                        uint32_t xferred) {
    if (ep_addr == s_ep_in) {
        if (result == XFER_RESULT_SUCCESS) g_xid_in_ok++; else g_xid_in_err++;
        arm_in(rhport);
    } else if (ep_addr == s_ep_out) {
        if (result == XFER_RESULT_SUCCESS) parse_rumble(s_out_buf, xferred);
        arm_out(rhport);
    }
    return true;
}

// SOF watchdog: completions are the primary pump; this only re-arms a pipe that
// went idle because a submit failed.
static void xid_sof(uint8_t rhport, uint32_t frame_count) {
    (void)frame_count;
    if (!s_configured || !s_usb_enabled || s_ota_quiesce) return;
    arm_in(rhport);
    arm_out(rhport);
}

static const usbd_class_driver_t s_xid_driver = {
    .name            = "talon-xid",
    .init            = xid_init,
    .deinit          = NULL,
    .reset           = xid_reset,
    .open            = xid_open,
    .control_xfer_cb = xid_control_xfer,
    .xfer_cb         = xid_xfer_cb,
    .xfer_isr        = NULL,
    .sof             = xid_sof,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &s_xid_driver;
}
