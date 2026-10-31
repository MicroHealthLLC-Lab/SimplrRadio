// SPDX-License-Identifier: Apache-2.0
//
// The radio's tasks that start and stop with each station (stream, mp3dec). On the ESP32-S3
// their 16 KB stacks go to PSRAM: its internal RAM can't hold them next to WiFi, TLS and the
// display. They never write flash or drive the SD card, which a PSRAM stack would forbid.
// Elsewhere they stay in internal RAM.

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

static inline BaseType_t task_stack_start(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                          UBaseType_t prio)
{
    return xTaskCreateWithCaps(fn, name, stack, arg, prio, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// The task's last call: ends it and frees its stack
static inline void task_stack_exit(void)
{
    vTaskDeleteWithCaps(NULL);
}

#else

static inline BaseType_t task_stack_start(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                          UBaseType_t prio)
{
    return xTaskCreate(fn, name, stack, arg, prio, NULL);
}

static inline void task_stack_exit(void)
{
    vTaskDelete(NULL);
}

#endif
