// SPDX-License-Identifier: Apache-2.0
//
// ESP-IDF's panic handler is wrapped (-Wl,--wrap=esp_panic_handler, main/CMakeLists.txt): the
// wrapper copies the crash's reason, task and address into RTC memory, which a restart keeps,
// then hands over to the real handler (which prints and restarts as before). On Xtensa (the
// T-Deck) the callers' addresses are kept too, to be looked up in the build's ELF. Nothing
// secret is kept: exception names, a task name and code addresses.

#include <stdio.h>
#include <string.h>

#include "crash_note.h"
#include "esp_attr.h"
#include "esp_debug_helpers.h"
#include "esp_private/cache_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "xtensa_context.h"
#endif

#define NOTE_MAGIC 0x52414443u   // "CDAR"

#define NOTE_CALLERS 6

typedef struct {
    uint32_t magic;
    uint32_t addr;
    uint32_t callers[NOTE_CALLERS];   // Return addresses, innermost first (0: none)
    char reason[48];
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
        // The exception's name ("InstrFetchProhibited"...); the description is generic
        copy_text(s_note.reason, sizeof(s_note.reason), info->reason ? info->reason : info->description);
        memset(s_note.callers, 0, sizeof(s_note.callers));
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        if (info->frame) {
            const XtExcFrame *f = (const XtExcFrame *)info->frame;
            esp_backtrace_frame_t frame = {.pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = f};
            for (int i = 0; i < NOTE_CALLERS && frame.next_pc; i++) {
                // A windowed return address: the top two bits are the window size, and it points
                // after the 3-byte call (as esp_backtrace_print shows it)
                s_note.callers[i] = ((frame.next_pc & 0x3FFFFFFFu) | 0x40000000u) - 3;
                if (!esp_backtrace_get_next_frame(&frame)) {
                    break;
                }
            }
        }
#endif
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
        int n = snprintf(out, len, "The radio restarted after a crash: %s (task %s, at 0x%08lx", s_note.reason,
                         s_note.task[0] ? s_note.task : "?", (unsigned long)s_note.addr);
        for (int i = 0; i < NOTE_CALLERS && s_note.callers[i] && n > 0 && (size_t)n < len; i++) {
            if (i == 0) {
                n += snprintf(out + n, len - n, ", from %08lx", (unsigned long)s_note.callers[i]);
            } else {
                n += snprintf(out + n, len - n, " %08lx", (unsigned long)s_note.callers[i]);
            }
        }
        if (n > 0 && (size_t)n < len) {
            snprintf(out + n, len - n, "%s", ").");
        }
    } else if (why == ESP_RST_PANIC) {
        snprintf(out, len, "%s", "The radio restarted after a crash.");
    } else if (why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT) {
        snprintf(out, len, "%s", "The radio restarted: something stopped responding (watchdog).");
    } else if (why == ESP_RST_BROWNOUT) {
        snprintf(out, len, "%s", "The radio restarted: the power dipped too low (brownout). Charge the battery.");
    }
}
