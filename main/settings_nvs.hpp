/*
 * Settings NVS
 * The NVS partition that holds every saved setting (WiFi networks, the vault, station search,
 * volume, last station). Flashed directly (release image at 0x0) it is "nvs", after the app.
 * Installed by a launcher (bmorcelli/Launcher, M5 Launcher) the device's "nvs" belongs to the
 * launcher, so the radio uses its own "webradio" data partition, which the launcher creates from
 * partitions_launcher.csv and keeps across updates. All settings code opens namespaces through
 * settings_nvs::open(). Adapted from SimplrSSH's settings_nvs.
 */

#ifndef SETTINGS_NVS_HPP
#define SETTINGS_NVS_HPP

#include "esp_err.h"
#include "nvs.h"

namespace settings_nvs
{
    enum class InitResult {
        OK,            // Settings partition ready
        REFORMATTED,   // Was unreadable and has been reset (own partition only)
        SHARED,        // From a launcher without the "webradio" partition: the launcher's own
                       // "nvs", which is never reformatted
        UNAVAILABLE,   // No usable settings partition: nothing can be saved
    };

    InitResult init();                     // Once at boot, before anything reads settings
    const char* partition();               // Label for nvs_entry_find() and friends
    esp_err_t open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* handle);
}

#endif
