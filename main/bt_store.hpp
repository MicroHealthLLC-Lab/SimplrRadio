/*
 * Bluetooth devices (bt_hid.cpp): which keyboards and remotes are paired, whether Bluetooth is
 * on, and their bonding keys.
 *
 * NVS namespace "bt" (settings partition):
 *   "on"     u8: 1 = Bluetooth on (off by default)
 *   "devs"   version (1) | count | count x (address type | address[6] | name length | name)
 * Bonding keys: vault ids "bt:<address type><12 hex digits>" (secret_vault.hpp), encrypted like
 * the WiFi passwords. NimBLE keeps no copy in flash of its own (CONFIG_BT_NIMBLE_NVS_PERSIST=n).
 * No ESP-IDF Bluetooth headers (host tests).
 */

#ifndef BT_STORE_HPP
#define BT_STORE_HPP

#include "esp_err.h"
#include <cstdint>
#include <string>
#include <vector>

namespace bt_store
{
    static const size_t MAX_DEVICES = 4;
    static const size_t MAX_NAME = 24;

    struct Device {
        uint8_t addr_type = 0;   // BLE identity address type (0 public, 1 random static)
        uint8_t addr[6] = {};    // As NimBLE stores it (least significant byte first)
        std::string name;        // Printable ASCII, at most MAX_NAME
    };

    bool same(const Device& a, uint8_t addr_type, const uint8_t addr[6]);

    bool enabled();
    esp_err_t set_enabled(bool on);

    // Paired devices, most recently paired first
    std::vector<Device> devices();
    // Adds (or moves to the front) a device; beyond MAX_DEVICES the oldest is forgotten
    esp_err_t remember(const Device& device);
    // Removes a device and its keys
    void forget(uint8_t addr_type, const uint8_t addr[6]);

    // Bonding keys of one device, as bt_hid.cpp serializes them (opaque here)
    esp_err_t put_keys(uint8_t addr_type, const uint8_t addr[6], const std::string& keys);
    bool get_keys(uint8_t addr_type, const uint8_t addr[6], std::string& keys);
    void remove_keys(uint8_t addr_type, const uint8_t addr[6]);
}

#endif
