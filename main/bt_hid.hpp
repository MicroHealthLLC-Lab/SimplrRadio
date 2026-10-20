/*
 * Bluetooth keyboards and remotes: the radio is a Bluetooth LE HID host (HID over GATT) on
 * NimBLE, central role only. It never advertises, offers no services and accepts no incoming
 * connections: it scans when the user asks, connects to the device the user picks, pairs
 * (LE Secure Connections, or legacy pairing for older keyboards; a keyboard types the 6-digit
 * code shown on the screen), and then turns the device's key reports into the radio's keys.
 * Paired devices (bt_store.hpp) are connected again by themselves whenever they come in range
 * while Bluetooth is on. Off by default.
 *
 * Both radios are Bluetooth LE only (ESP32-S3; the Tab5's ESP32-C6, reached over esp_hosted):
 * Classic Bluetooth, and with it headphones and speakers (A2DP), is not possible on them.
 *
 * Threads: start(), stop() and scan() block (worker task). pair(), forget(), read_key() and
 * the getters are for the app task. NimBLE's host task runs the callbacks.
 */

#ifndef BT_HID_HPP
#define BT_HID_HPP

#include "bt_store.hpp"
#include "esp_err.h"
#include <cstdint>
#include <string>
#include <vector>

namespace bt_hid
{
    static const size_t MAX_FOUND = 12;   // Devices listed by one scan

    enum class State : uint8_t {
        Off,           // Not started
        Unavailable,   // Start failed (no Bluetooth controller)
        Idle,          // On; waiting() tells whether it listens for a paired device
        Scanning,
        Connecting,    // To the device the user picked
        Pairing,       // Encrypting the link; a code may be shown (passkey())
        Setup,         // Reading the device's key reports
        Connected,
    };

    struct Found {
        uint8_t addr_type = 0;
        uint8_t addr[6] = {};
        std::string name;    // Printable ASCII; "" when the device sent none
        int rssi = -127;
    };

    esp_err_t start();
    void stop();

    State state();
    bool waiting();                  // Listening for a paired device to come back
    std::string device();            // The connected (or connecting) device's name
    bool is_connected(const bt_store::Device& device);
    bool passkey(uint32_t& code);    // The code to type on the keyboard, while pairing
    std::string take_message();      // A result to show once ("Paired with ...")
    uint32_t version();              // Changes whenever anything above changes

    esp_err_t scan(std::vector<Found>& out);   // About 8 s; keyboards and remotes, strongest first
    esp_err_t pair(const Found& device);       // Connects and pairs; the outcome via state()
    void forget(const bt_store::Device& device);

    uint32_t read_key();   // 0 when no key is waiting, else a keys.h value
}

#endif
