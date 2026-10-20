// SPDX-License-Identifier: Apache-2.0
//
// ESP32-C6 WiFi co-processor firmware update from the image embedded in the app.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*slave_ota_progress_cb_t)(int percent);

// After esp_hosted_connect_to_slave(): updates the C6 if its version differs from the embedded
// image (first boot on most boards, about 30 s; `progress` reports 0-100). A no-op afterwards.
esp_err_t slave_ota_update_if_needed(slave_ota_progress_cb_t progress);

// The co-processor's firmware version as last read ("unknown" before the first check).
const char *slave_ota_version(void);

#ifdef __cplusplus
}
#endif
