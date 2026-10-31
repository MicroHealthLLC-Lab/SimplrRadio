// SPDX-License-Identifier: Apache-2.0
//
// ESP-IDF's panic handler is wrapped (-Wl,--wrap=esp_panic_handler, main/CMakeLists.txt): the
// wrapper copies the crash's reason, task and address into RTC memory, which a restart keeps,
// then hands over to the real handler (which prints and restarts as before). Nothing secret is
// kept: exception names, a task name and an address.

#include <stdio.h>
#include <string.h>

#include "crash_note.h"
#include "esp_attr.h"
#include "esp_private/cache_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NOTE_MAGIC 0x52414443u   // "CDAR"

typedef struct {
    uint32_t magic;
    uint32_t addr;
    char reason[64];
    char task[16];
} crash_note_t;

static RTC_NOINIT_ATTR crash_note_t s_note;

void __real_esp_panic_handler(panic_info_t *info);

// Runs in the panic handler: IRAM, no allocation, and flash strings only while the cache is on
static void IRAM_ATTR copy_text(char *dst, size_t len, const char *src)
{
    size_t i = 0;
    if (src && spi_flash_cache_enabled()) {
        for (; i + 1 < len && src[i]; i++) {
            char c = src[i];
            dst[i] = c >= 0x20 && c < 0x7F ? c : '?';
        }
    }
    dst[i] = '\0';
}

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    if (info) {
        s_note.magic = 0;
        s_note.addr = (uint32_t)(uintptr_t)info->addr;
        copy_text(s_note.reason, sizeof(s_note.reason), info->description ? info->description : info->reason);
        TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(info->core);
        copy_text(s_note.task, sizeof(s_note.task), task ? pcTaskGetName(task) : NULL);
        s_note.magic = NOTE_MAGIC;
    }
    __real_esp_panic_handler(info);
}

void crash_note_take(char *out, size_t len)
{
    out[0] = '\0';
    esp_reset_reason_t why = esp_reset_reason();
    bool noted = s_note.magic == NOTE_MAGIC;
    s_note.magic = 0;
    if (why == ESP_RST_PANIC && noted) {
        s_note.reason[sizeof(s_note.reason) - 1] = '\0';
        s_note.task[sizeof(s_note.task) - 1] = '\0';
        snprintf(out, len, "The radio restarted after a crash: %s (task %s, at 0x%08lx).", s_note.reason,
                 s_note.task[0] ? s_note.task : "?", (unsigned long)s_note.addr);
    } else if (why == ESP_RST_PANIC) {
        snprintf(out, len, "%s", "The radio restarted after a crash.");
    } else if (why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT) {
        snprintf(out, len, "%s", "The radio restarted: something stopped responding (watchdog).");
    } else if (why == ESP_RST_BROWNOUT) {
        snprintf(out, len, "%s", "The radio restarted: the power dipped too low (brownout). Charge the battery.");
    }
}
