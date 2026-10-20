/*
 * LVGL Memory
 * LVGL's allocator (CONFIG_LV_USE_CUSTOM_MALLOC): widgets, styles and texts live in PSRAM,
 * so menu pages and station lists don't take the internal RAM that WiFi, TLS and audio need.
 * Falls back to internal RAM only if PSRAM is full. The display's draw buffers are not
 * allocated here (esp_lvgl_port places them for DMA itself).
 *
 * Shared with SimplrSSH (github.com/MicroHealthLLC/SimplrSSH, MIT).
 */

#include "lvgl.h"
#include "esp_heap_caps.h"

static const uint32_t PSRAM_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;   // No pools: everything comes from the heap
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void* lv_malloc_core(size_t size)
{
    void* p = heap_caps_malloc(size, PSRAM_CAPS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void* lv_realloc_core(void* p, size_t new_size)
{
    void* q = heap_caps_realloc(p, new_size, PSRAM_CAPS);
    return q ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void* p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p)
{
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
