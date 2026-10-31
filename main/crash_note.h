// SPDX-License-Identifier: Apache-2.0
//
// Why the radio last restarted, for the screen: a crash's reason, task and address are kept
// across the restart (RTC memory), so a crash can be read without a serial cable.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// After a crash, watchdog or brownout restart: one line saying so (printable ASCII), and the
// note is cleared. "" after a normal start.
void crash_note_take(char *out, size_t len);

#ifdef __cplusplus
}
#endif
