// SPDX-License-Identifier: Apache-2.0
//
// HID report map parser and input report decoder (hid_report.h). Short items only (long items
// are skipped, as the HID spec defines none); globals with push/pop, locals cleared by every
// main item. Only Input main items move the field offsets.

#include "hid_report.h"
#include <string.h>

#define MAX_IDS      16     // Report IDs counted in one map
#define STACK_DEPTH  4      // Push / Pop levels
#define MAX_BITS     4096   // A larger report is malformed

typedef struct {
    uint16_t usage_page;
    int32_t logical_min;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
} globals_t;

typedef struct {
    uint16_t usages[HID_MAX_USAGES];
    uint8_t n_usages;
    uint16_t usage_min;
    uint16_t usage_max;
    bool range;
    uint16_t page;           // From an extended (32-bit) usage, 0 = the global usage page
} locals_t;

typedef struct {
    uint8_t id;
    uint32_t bits;
} offset_t;

static uint32_t item_value(const uint8_t *p, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        v |= (uint32_t)p[i] << (8 * i);
    }
    return v;
}

static int32_t item_signed(const uint8_t *p, int size)
{
    uint32_t v = item_value(p, size);
    if (size == 1) {
        return (int8_t)v;
    }
    if (size == 2) {
        return (int16_t)v;
    }
    return (int32_t)v;
}

static uint32_t *offset_for(offset_t *table, int *n, uint8_t id)
{
    for (int i = 0; i < *n; i++) {
        if (table[i].id == id) {
            return &table[i].bits;
        }
    }
    if (*n == MAX_IDS) {
        return NULL;
    }
    table[*n].id = id;
    table[*n].bits = 0;
    return &table[(*n)++].bits;
}

static void add_field(hid_map_t *map, const globals_t *g, const locals_t *l, uint32_t offset, uint32_t flags)
{
    uint16_t page = l->page ? l->page : g->usage_page;
    if ((flags & 1) || g->report_size < 1 || g->report_size > 16 || g->report_count == 0 ||
        (page != HID_PAGE_KEYBOARD && page != HID_PAGE_CONSUMER) || (!l->n_usages && !l->range) ||
        map->count >= HID_MAX_FIELDS) {
        return;   // Constant padding, a size the radio doesn't decode, or a page it doesn't use
    }
    hid_field_t *f = &map->fields[map->count++];
    memset(f, 0, sizeof(*f));
    f->report_id = g->report_id;
    f->usage_page = page;
    f->bit_offset = (uint16_t)offset;
    f->bit_size = (uint8_t)g->report_size;
    f->count = g->report_count > 255 ? 255 : (uint8_t)g->report_count;
    f->variable = (flags & 2) != 0;
    f->logical_min = g->logical_min;
    if (l->range) {
        f->usage_min = l->usage_min;
        f->usage_max = l->usage_max;
    }
    for (int i = 0; i < l->n_usages; i++) {
        f->usages[i] = l->usages[i];
    }
    f->n_usages = l->n_usages;
}

bool hid_parse_map(const uint8_t *desc, size_t len, hid_map_t *map)
{
    memset(map, 0, sizeof(*map));
    if (!desc || len == 0 || len > HID_MAX_MAP_BYTES) {
        return false;
    }
    globals_t g = {0};
    globals_t stack[STACK_DEPTH];
    int depth = 0;
    locals_t l = {0};
    offset_t offsets[MAX_IDS] = {{0, 0}};
    int n_offsets = 0;

    size_t i = 0;
    while (i < len) {
        uint8_t b = desc[i];
        if (b == 0xFE) {   // Long item: bDataSize, bLongItemTag, data
            if (i + 1 >= len) {
                return false;
            }
            i += 3 + (size_t)desc[i + 1];
            continue;
        }
        int size = b & 3;
        if (size == 3) {
            size = 4;
        }
        if (i + 1 + (size_t)size > len) {
            return false;
        }
        const uint8_t *d = desc + i + 1;
        uint32_t v = item_value(d, size);
        int type = (b >> 2) & 3;
        int tag = b >> 4;
        if (type == 0) {   // Main
            if (tag == 8) {   // Input
                uint32_t *off = offset_for(offsets, &n_offsets, g.report_id);
                uint32_t bits = g.report_size * g.report_count;
                if (!off || g.report_size > 32 || g.report_count > MAX_BITS || *off + bits > MAX_BITS) {
                    return false;
                }
                add_field(map, &g, &l, *off, v);
                *off += bits;
            }
            memset(&l, 0, sizeof(l));   // Every main item ends the local items
        } else if (type == 1) {   // Global
            switch (tag) {
                case 0: g.usage_page = (uint16_t)v; break;
                case 1: g.logical_min = item_signed(d, size); break;
                case 7: g.report_size = v; break;
                case 8:
                    if (v == 0 || v > 255) {
                        return false;   // Report ID 0 is reserved
                    }
                    g.report_id = (uint8_t)v;
                    map->has_ids = true;
                    break;
                case 9: g.report_count = v; break;
                case 10:
                    if (depth < STACK_DEPTH) {
                        stack[depth++] = g;
                    }
                    break;
                case 11:
                    if (depth > 0) {
                        g = stack[--depth];
                    }
                    break;
                default: break;
            }
        } else if (type == 2) {   // Local
            if (size == 4 && tag <= 2) {
                l.page = (uint16_t)(v >> 16);   // Extended usage: page in the high half
            }
            switch (tag) {
                case 0:
                    if (l.n_usages < HID_MAX_USAGES) {
                        l.usages[l.n_usages++] = (uint16_t)v;
                    }
                    break;
                case 1: l.usage_min = (uint16_t)v; l.range = true; break;
                case 2: l.usage_max = (uint16_t)v; l.range = true; break;
                default: break;
            }
        }
        i += 1 + (size_t)size;
    }
    return map->count > 0;
}

bool hid_map_uses(const hid_map_t *map, uint8_t report_id)
{
    for (int i = 0; i < map->count; i++) {
        if (!map->has_ids || map->fields[i].report_id == report_id) {
            return true;
        }
    }
    return false;
}

static uint32_t bits_at(const uint8_t *data, uint32_t offset, uint8_t size)
{
    uint32_t v = 0;
    for (uint8_t k = 0; k < size; k++) {
        uint32_t bit = offset + k;
        if ((data[bit / 8] >> (bit % 8)) & 1) {
            v |= 1u << k;
        }
    }
    return v;
}

static void add_key(hid_input_t *out, uint32_t usage)
{
    if (usage >= 0xE0 && usage <= 0xE7) {
        out->modifiers |= (uint8_t)(1u << (usage - 0xE0));
        return;
    }
    if (usage < 0x04 || usage > 0xFF) {
        return;   // 0 none, 1-3 error codes (too many keys held)
    }
    for (int i = 0; i < out->n_keys; i++) {
        if (out->keys[i] == usage) {
            return;
        }
    }
    if (out->n_keys < HID_MAX_KEYS) {
        out->keys[out->n_keys++] = (uint8_t)usage;
    }
}

static void add_consumer(hid_input_t *out, uint32_t usage)
{
    if (usage == 0 || usage > 0xFFFF) {
        return;
    }
    for (int i = 0; i < out->n_consumer; i++) {
        if (out->consumer[i] == usage) {
            return;
        }
    }
    if (out->n_consumer < HID_MAX_CONSUMER) {
        out->consumer[out->n_consumer++] = (uint16_t)usage;
    }
}

void hid_decode(const hid_map_t *map, uint8_t report_id, const uint8_t *data, size_t len, hid_input_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!data) {
        return;
    }
    if (len > HID_MAX_REPORT) {
        len = HID_MAX_REPORT;
    }
    for (int i = 0; i < map->count; i++) {
        const hid_field_t *f = &map->fields[i];
        if (map->has_ids && f->report_id != report_id) {
            continue;
        }
        for (uint32_t e = 0; e < f->count; e++) {
            uint32_t off = f->bit_offset + e * f->bit_size;
            if (off + f->bit_size > len * 8) {
                break;   // Shorter report than the map says
            }
            uint32_t raw = bits_at(data, off, f->bit_size);
            uint32_t usage;
            if (f->variable) {
                if (f->bit_size != 1 || !raw) {
                    continue;   // Only on/off controls are keys
                }
                if (e < f->n_usages) {
                    usage = f->usages[e];
                } else if (f->usage_max) {
                    usage = f->usage_min + e;
                    if (usage > f->usage_max) {
                        continue;
                    }
                } else if (f->n_usages) {
                    usage = f->usages[f->n_usages - 1];   // The last usage applies to the rest
                } else {
                    continue;
                }
            } else {
                int32_t index = (int32_t)raw - f->logical_min;
                if (f->usage_max || f->usage_min) {
                    if (index < 0 || (uint32_t)f->usage_min + (uint32_t)index > f->usage_max) {
                        continue;   // Outside the range: nothing pressed
                    }
                    usage = f->usage_min + (uint32_t)index;
                } else if (index >= 0 && index < f->n_usages) {
                    usage = f->usages[index];
                } else {
                    continue;
                }
            }
            if (f->usage_page == HID_PAGE_KEYBOARD) {
                add_key(out, usage);
            } else {
                add_consumer(out, usage);
            }
        }
    }
}

void hid_decode_boot_keyboard(const uint8_t *data, size_t len, hid_input_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!data || len < 3) {
        return;
    }
    out->modifiers = data[0];
    for (size_t i = 2; i < len && i < 8; i++) {
        add_key(out, data[i]);
    }
}
