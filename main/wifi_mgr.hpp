/*
 * WiFi Manager
 * The WiFi link through the ESP32-C6 co-processor (esp_hosted + esp_wifi_remote) and the saved
 * networks, as SimplrSSH does it: scan (strongest first), connect, remember every network that
 * connects (most recent first, passwords encrypted in the vault), join the strongest saved
 * network in range, forget. The C6 keeps no copy of any password (WIFI_STORAGE_RAM).
 *
 * The blocking calls (start, scan, connect, auto_connect) belong in a worker task, never the
 * UI's. The rest is cheap and safe from any task.
 */

#ifndef WIFI_MGR_HPP
#define WIFI_MGR_HPP

#include "esp_err.h"
#include <cstdint>
#include <string>
#include <vector>

namespace wifi
{
    static const size_t MAX_SAVED = 10;

    struct Network {             // A scan result
        std::string ssid;
        int rssi = -100;
        bool open = false;
    };

    struct Saved {
        std::string ssid;
        bool has_password = false;   // Its password is in the vault (false: open network)
        bool hidden = false;         // Not broadcast: tried even when a scan doesn't list it
    };

    typedef void (*progress_cb_t)(const char* status);   // Boot progress for the screen
    typedef void (*link_cb_t)(bool up);                   // Link came up / went down

    // Links to the co-processor, updates its firmware when needed, starts the driver in station
    // mode. Idempotent. Blocking (first boot with a C6 update: about 30 s).
    esp_err_t start(progress_cb_t progress);
    bool started();
    void on_link(link_cb_t cb);

    esp_err_t scan(std::vector<Network>& out);   // De-duplicated by SSID, strongest first

    // Joins `ssid`; `error` says why not, in words for the screen. Blocking (up to ~20 s).
    esp_err_t connect(const std::string& ssid, const std::string& password, std::string& error);
    void disconnect();

    bool connected();
    std::string ssid();          // The network joined, "" if none
    std::string ip();
    int rssi();

    // Saved networks, most recently used first
    std::vector<Saved> saved();
    bool is_saved(const std::string& ssid);
    // After a successful connect: saves it (password into the vault) at the top of the list,
    // dropping the least recently used beyond MAX_SAVED
    esp_err_t remember(const std::string& ssid, const std::string& password, bool hidden);
    void forget(const std::string& ssid);
    bool saved_password(const std::string& ssid, std::string& password);

    // Joins the strongest saved network in range (then saved hidden ones). `joined` names it.
    // Blocking. ESP_ERR_NOT_FOUND: nothing saved is in range.
    esp_err_t auto_connect(std::string& joined, std::string& error);
}

#endif
