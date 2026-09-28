// Talon — generic HID gamepad report parser (see bt_hidmap.h).
#include <string.h>
#include <math.h>
#include "bt_hidmap.h"
#include "esp_log.h"

static const char *TAG = "talon.hid";

// HID Usage Pages / usages we care about.
#define PAGE_GENERIC_DESKTOP 0x01
#define PAGE_SIMULATION      0x02
#define PAGE_BUTTON          0x09
#define GD_X    0x30
#define GD_Y    0x31
#define GD_Z    0x32
#define GD_RX   0x33
#define GD_RY   0x34
#define GD_RZ   0x35
#define GD_HAT  0x39
#define SIM_RT  0xC4   // Simulation Controls: right trigger
#define SIM_LT  0xC5   // Simulation Controls: left trigger

// Talon state indices.
//enum { S_DIGITAL, S_A, S_B, S_X, S_Y, S_BLACK, S_WHITE, S_LT, S_RT,
//       S_LX, S_LY, S_RX, S_RY };

// Digital d-pad/START/BACK/thumb bits (byte 2 of the XID report).
//#define D_UP 0x01
//#define D_DOWN 0x02
//#define D_LEFT 0x04
//#define D_RIGHT 0x08
//#define D_START 0x10
//#define D_BACK 0x20
//#define D_LS 0x40
//#define D_RS 0x80

#define STICK_DEADZONE 0.08
#define CALIBRATION_SAMPLES 8

typedef struct {
    uint16_t usage_page;
    int32_t  logical_min, logical_max;
    uint8_t  report_size, report_id;
    uint16_t report_count;
} global_state_t;

static int s_state[13];
static bool s_calibrating;
static uint8_t s_cal_samples;
static int64_t s_cal_sum[4];
static uint8_t s_cal_count[4];
static int32_t s_cal_center[4];

static uint32_t item_uval(const uint8_t *p, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t item_sval(const uint8_t *p, int size) {
    uint32_t u = item_uval(p, size);
    int bits = size * 8;
    if (bits < 32 && (u & (1u << (bits - 1)))) u |= ~((1u << bits) - 1u);
    return (int32_t)u;
}

void hid_mapping_reset(void) {
    memset(s_state, 0, sizeof(s_state));
    memset(s_cal_sum, 0, sizeof(s_cal_sum));
    memset(s_cal_count, 0, sizeof(s_cal_count));
    memset(s_cal_center, 0, sizeof(s_cal_center));
    s_calibrating = true;
    s_cal_samples = 0;
}

bool hid_parse_descriptor(const uint8_t *desc, size_t len, hid_layout_t *out) {
    memset(out, 0, sizeof(*out));
    hid_mapping_reset();

    global_state_t g = { 0 };
    uint16_t usages[16]; int n_usages = 0;
    uint16_t usage_min = 0, usage_max = 0; bool have_range = false;
    uint16_t bitpos[8] = { 0 };
    uint8_t ids[8]; int n_ids = 0;

    size_t i = 0;
    while (i < len) {
        uint8_t b = desc[i++];
        if (b == 0xFE) {
            if (i >= len) break;
            uint8_t dsize = desc[i];
            i += 2 + dsize;
            continue;
        }
        int size = b & 0x03; if (size == 3) size = 4;
        int type = (b >> 2) & 0x03;
        int tag  = (b >> 4) & 0x0F;
        if (i + size > len) break;
        const uint8_t *data = &desc[i];
        i += size;

        if (type == 1) {
            switch (tag) {
                case 0x0: g.usage_page = (uint16_t)item_uval(data, size); break;
                case 0x1: g.logical_min = item_sval(data, size); break;
                case 0x2: g.logical_max = item_sval(data, size); break;
                case 0x7: g.report_size = (uint8_t)item_uval(data, size); break;
                case 0x8: g.report_id = (uint8_t)item_uval(data, size); out->has_report_id = true; break;
                case 0x9: g.report_count = (uint16_t)item_uval(data, size); break;
                default: break;
            }
        } else if (type == 2) {
            switch (tag) {
                case 0x0: if (n_usages < 16) usages[n_usages++] = (uint16_t)item_uval(data, size); break;
                case 0x1: usage_min = (uint16_t)item_uval(data, size); have_range = true; break;
                case 0x2: usage_max = (uint16_t)item_uval(data, size); have_range = true; break;
                default: break;
            }
        } else if (type == 0) {
            if (tag == 0x8) {
                uint8_t flags = size ? data[0] : 0;
                bool constant = flags & 0x01;
                int slot = 0;
                for (; slot < n_ids; slot++) if (ids[slot] == g.report_id) break;
                if (slot == n_ids && n_ids < 8) { ids[n_ids] = g.report_id; n_ids++; }
                uint16_t *cursor = &bitpos[slot < 8 ? slot : 0];
                for (uint16_t f = 0; f < g.report_count; f++) {
                    if (!constant && out->n < HID_MAX_FIELDS) {
                        uint16_t u;
                        if (n_usages > 0) u = usages[f < n_usages ? f : n_usages - 1];
                        else if (have_range) u = usage_min + f;
                        else u = 0;
                        hid_field_t *fld = &out->fields[out->n++];
                        fld->report_id = g.report_id;
                        fld->bit_offset = *cursor;
                        fld->bit_size = g.report_size;
                        fld->usage_page = g.usage_page;
                        fld->usage = u;
                        fld->logical_min = g.logical_min;
                        fld->logical_max = g.logical_max;
                    }
                    *cursor += g.report_size;
                }
            }
            n_usages = 0; usage_min = usage_max = 0; have_range = false;
        }
    }
    return out->n > 0;
}

void hid_log_layout(const hid_layout_t *l) {
    ESP_LOGI(TAG, "HID layout: %d fields, report IDs=%s", l->n, l->has_report_id ? "yes" : "no");
    for (int i = 0; i < l->n; ++i) {
        const hid_field_t *f = &l->fields[i];
        ESP_LOGI(TAG, "field[%d]: id=%u page=0x%04x usage=0x%04x off=%u size=%u logical=%ld..%ld",
                 i, f->report_id, f->usage_page, f->usage, f->bit_offset, f->bit_size,
                 (long)f->logical_min, (long)f->logical_max);
    }
}

static uint32_t extract_bits(const uint8_t *data, size_t len, uint16_t off, uint8_t sz) {
    uint32_t v = 0;
    if (sz > 32) sz = 32;
    for (uint8_t b = 0; b < sz; b++) {
        uint16_t bit = off + b;
        if (bit / 8 >= len) break;
        if (data[bit / 8] & (1u << (bit % 8))) v |= (1u << b);
    }
    return v;
}

static int32_t decode_raw(uint32_t raw, uint8_t bits, int32_t logical_min) {
    if (bits == 0) return 0;
    if (bits > 31 || logical_min >= 0) return (int32_t)raw;
    uint32_t sign = 1u << (bits - 1);
    uint32_t mask = (1u << bits) - 1u;
    raw &= mask;
    if (raw & sign) raw |= ~mask;
    return (int32_t)raw;
}

static int scale_axis(int32_t raw, int32_t lmin, int32_t lmax, int32_t center) {
    if (lmax <= lmin) return 0;
    double d = (double)(raw - center);
    double n = d >= 0.0 ? d / (double)(lmax - center) : d / (double)(center - lmin);
    if (n > 1.0) n = 1.0;
    if (n < -1.0) n = -1.0;
    if (fabs(n) < STICK_DEADZONE) return 0;
    double sign = n < 0 ? -1.0 : 1.0;
    double mag = (fabs(n) - STICK_DEADZONE) / (1.0 - STICK_DEADZONE);
    return (int)(sign * mag * 32767.0);
}

static uint8_t scale_u8(int32_t raw, int32_t lmin, int32_t lmax) {
    if (lmax <= lmin) return raw > lmin ? 255 : 0;
    double t = ((double)raw - (double)lmin) / (double)(lmax - lmin);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (uint8_t)(t * 255.0 + 0.5);
}

static uint8_t hat_to_dpad(int32_t h, int32_t lmin, int32_t lmax) {
    // HID null-state is commonly logical_max+1 (e.g. 8 for a 0..7 hat).
    if (h < lmin || h > lmax || (lmax - lmin) < 7) return 0;
    uint32_t v = (uint32_t)(h - lmin);
    if (v >= 8) return 0;
    switch (v) {
        case 0: return D_UP;
        case 1: return D_UP | D_RIGHT;
        case 2: return D_RIGHT;
        case 3: return D_DOWN | D_RIGHT;
        case 4: return D_DOWN;
        case 5: return D_DOWN | D_LEFT;
        case 6: return D_LEFT;
        case 7: return D_UP | D_LEFT;
        default: return 0;
    }
}

// Xbox Series X|S BLE controllers use a sparse 15-bit button field.
// The HID descriptor advertises Usage Minimum 1 .. Maximum 15, but usages
// 3, 6 and 9 are reserved.  Do NOT treat the usage number as a compact
// 0..9 button index:
//   1 A, 2 B, 4 X, 5 Y, 7 LB, 8 RB, 11 Back, 12 Start, 14 LS, 15 RS.
// This sparse layout is why the old compact mapping produced the observed
// Y->White, X->Y, White->Back and Black->Start shifts.
static void apply_button(int btn, uint32_t pressed, int state[13]) {
    switch (btn) {
        case 1:  state[S_A] = pressed ? 255 : 0; break;
        case 2:  state[S_B] = pressed ? 255 : 0; break;
        case 4:  state[S_X] = pressed ? 255 : 0; break;
        case 5:  state[S_Y] = pressed ? 255 : 0; break;
        case 7:  state[S_WHITE] = pressed ? 255 : 0; break; // LB
        case 8:  state[S_BLACK] = pressed ? 255 : 0; break; // RB
        case 11: if (pressed) state[S_DIGITAL] |= D_BACK;  else state[S_DIGITAL] &= ~D_BACK; break;
        case 12: if (pressed) state[S_DIGITAL] |= D_START; else state[S_DIGITAL] &= ~D_START; break;
        case 14: if (pressed) state[S_DIGITAL] |= D_LS;    else state[S_DIGITAL] &= ~D_LS; break;
        case 15: if (pressed) state[S_DIGITAL] |= D_RS;    else state[S_DIGITAL] &= ~D_RS; break;
        default: break; // reserved usages 3, 6, 9 and 14, 15
    }
}

static int axis_slot(uint16_t page, uint16_t usage) {
    if (page != PAGE_GENERIC_DESKTOP) return -1;
    switch (usage) {
        case GD_X: return 0;
        case GD_Y: return 1;
        case GD_Z: return 2;
        case GD_RZ: return 3;
        default: return -1;
    }
}

static void calibrate_axis(int32_t raw, int slot) {
    if (!s_calibrating || slot < 0 || slot >= 4) return;
    s_cal_sum[slot] += raw;
    s_cal_count[slot]++;
}

bool hid_report_to_state(const hid_layout_t *l, uint8_t report_id,
                         const uint8_t *data, size_t len, int state[13]) {
    bool any = false;
    int32_t axis_raw[4] = {0};
    bool axis_seen[4] = {false};

    // First collect stick samples. No movement is emitted until calibration ends.
    if (s_calibrating) {
        for (int k = 0; k < l->n; k++) {
            const hid_field_t *f = &l->fields[k];
            if (l->has_report_id && f->report_id != report_id) continue;
            int slot = axis_slot(f->usage_page, f->usage);
            if (slot >= 0) {
                axis_raw[slot] = decode_raw(extract_bits(data, len, f->bit_offset, f->bit_size), f->bit_size, f->logical_min);
                axis_seen[slot] = true;
                calibrate_axis(axis_raw[slot], slot);
            }
        }
        if (axis_seen[0] || axis_seen[1] || axis_seen[2] || axis_seen[3]) {
            if (++s_cal_samples >= CALIBRATION_SAMPLES) {
                for (int i = 0; i < 4; i++)
                    s_cal_center[i] = s_cal_count[i] ? (int32_t)(s_cal_sum[i] / s_cal_count[i]) : 0;
                s_calibrating = false;
                ESP_LOGI(TAG, "stick calibration complete: X=%ld Y=%ld Z=%ld RZ=%ld",
                         (long)s_cal_center[0], (long)s_cal_center[1],
                         (long)s_cal_center[2], (long)s_cal_center[3]);
            }
        }
        // Buttons/hat/triggers are still valid during calibration; only stick
        // movement is suppressed until the center is established.
        bool non_axis = false;
        for (int k = 0; k < l->n; k++) {
            const hid_field_t *f = &l->fields[k];
            if (l->has_report_id && f->report_id != report_id) continue;
            uint32_t raw_u = extract_bits(data, len, f->bit_offset, f->bit_size);
            int32_t raw = decode_raw(raw_u, f->bit_size, f->logical_min);
            if (f->usage_page == PAGE_BUTTON) {
                apply_button(f->usage, raw_u, s_state);
                non_axis = true;
            } else if (f->usage_page == PAGE_GENERIC_DESKTOP && f->usage == GD_HAT) {
                uint8_t mask = hat_to_dpad(raw, f->logical_min, f->logical_max);
                s_state[S_DIGITAL] &= ~(D_UP|D_DOWN|D_LEFT|D_RIGHT);
                s_state[S_DIGITAL] |= mask;
                non_axis = true;
            } else if (f->usage_page == PAGE_SIMULATION) {
                if (f->usage == SIM_LT) { s_state[S_LT] = scale_u8(raw, f->logical_min, f->logical_max); non_axis = true; }
                else if (f->usage == SIM_RT) { s_state[S_RT] = scale_u8(raw, f->logical_min, f->logical_max); non_axis = true; }
            }
        }
        memcpy(state, s_state, sizeof(s_state));
        return non_axis;
    }

    for (int k = 0; k < l->n; k++) {
        const hid_field_t *f = &l->fields[k];
        if (l->has_report_id && f->report_id != report_id) continue;
        uint32_t raw_u = extract_bits(data, len, f->bit_offset, f->bit_size);
        int32_t raw = decode_raw(raw_u, f->bit_size, f->logical_min);

        if (f->usage_page == PAGE_BUTTON) {
            apply_button(f->usage, raw_u, s_state);
            any = true;
        } else if (f->usage_page == PAGE_GENERIC_DESKTOP) {
            int slot = axis_slot(f->usage_page, f->usage);
            switch (f->usage) {
                case GD_X:  s_state[S_LX] = scale_axis(raw, f->logical_min, f->logical_max, s_cal_center[slot]); any = true; break;
                case GD_Y:  s_state[S_LY] = -scale_axis(raw, f->logical_min, f->logical_max, s_cal_center[slot]); any = true; break;
                case GD_Z:  s_state[S_RX] = scale_axis(raw, f->logical_min, f->logical_max, s_cal_center[slot]); any = true; break;
                case GD_RZ: s_state[S_RY] = -scale_axis(raw, f->logical_min, f->logical_max, s_cal_center[slot]); any = true; break;
                case GD_HAT: {
                    uint8_t mask = hat_to_dpad(raw, f->logical_min, f->logical_max);
                    s_state[S_DIGITAL] &= ~(D_UP|D_DOWN|D_LEFT|D_RIGHT);
                    s_state[S_DIGITAL] |= mask;
                    any = true;
                    break;
                }
                default: break;
            }
        } else if (f->usage_page == PAGE_SIMULATION) {
            // Xbox BLE descriptors commonly put 10-bit triggers on Simulation Controls.
            if (f->usage == SIM_LT) { s_state[S_LT] = scale_u8(raw, f->logical_min, f->logical_max); any = true; }
            else if (f->usage == SIM_RT) { s_state[S_RT] = scale_u8(raw, f->logical_min, f->logical_max); any = true; }
        }
    }

    memcpy(state, s_state, sizeof(s_state));
    return any;
}

void hid_get_state(int state[13])
{
    memcpy(state, s_state, sizeof(s_state));
}
