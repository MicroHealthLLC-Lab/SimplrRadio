/*
 * WiFi Manager Implementation (wifi_mgr.hpp)
 *
 * NVS namespace "wifi" (settings partition):
 *   "nets"   version (1) | count | count x (flags | ssid length | ssid bytes)
 *            flags bit 0: password in the vault, bit 1: hidden network
 * Passwords: vault ids "wifi:<ssid>" (secret_vault.hpp).
 */

#include "wifi_mgr.hpp"
#include "board.h"
#include "secret_vault.hpp"
#include "settings_nvs.hpp"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

static const char *TAG = "wifi";

static const EventBits_t BIT_CONNECTED = BIT0;
static const EventBits_t BIT_FAILED = BIT1;
static const int CONNECT_TIMEOUT_MS = 20000;
static const int CONNECT_RETRIES = 2;        // Extra association attempts within one connect()
static const size_t SCAN_MAX = 30;

enum class State { Idle, Connecting, Connected };

static EventGroupHandle_t s_events;
static esp_netif_t* s_netif;
static volatile bool s_started;
static volatile bool s_power_save;           // Modem sleep (only while Bluetooth runs)
static volatile State s_state = State::Idle;
static volatile int s_retries;
static volatile uint8_t s_last_reason;
static char s_target[33];                   // SSID of the connect() in progress
static wifi::link_cb_t s_link_cb;
static std::string s_ssid;
static std::string s_ip;
static std::vector<wifi::Saved> s_saved;
static bool s_saved_loaded;

// Guards s_saved, s_ssid, s_ip; usable before start() (the UI reads saved networks at boot)
static SemaphoreHandle_t lock_handle()
{
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    return mutex;
}

struct Lock {
    Lock() { xSemaphoreTake(lock_handle(), portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(lock_handle()); }
};

// ---------------------------------------------------------------------------
// Saved networks
// ---------------------------------------------------------------------------

static std::string secret_id(const std::string& ssid)
{
    return "wifi:" + ssid;
}

// Lock held
static void load_saved()
{
    if (s_saved_loaded) {
        return;
    }
    s_saved_loaded = true;
    s_saved.clear();
    nvs_handle_t h;
    if (settings_nvs::open("wifi", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t buf[2 + wifi::MAX_SAVED * (2 + 32)];
    size_t len = sizeof(buf);
    if (nvs_get_blob(h, "nets", buf, &len) == ESP_OK && len >= 2 && buf[0] == 1) {
        size_t off = 2;
        for (int i = 0; i < buf[1] && s_saved.size() < wifi::MAX_SAVED; i++) {
            if (off + 2 > len || buf[off + 1] == 0 || buf[off + 1] > 32 || off + 2 + buf[off + 1] > len) {
                ESP_LOGW(TAG, "Saved networks record damaged; keeping %u", (unsigned)s_saved.size());
                break;
            }
            wifi::Saved n;
            n.has_password = buf[off] & 1;
            n.hidden = buf[off] & 2;
            n.ssid.assign((const char*)buf + off + 2, buf[off + 1]);
            s_saved.push_back(n);
            off += 2 + buf[off + 1];
        }
    }
    nvs_close(h);
}

// Lock held
static esp_err_t store_saved()
{
    uint8_t buf[2 + wifi::MAX_SAVED * (2 + 32)];
    size_t off = 2;
    uint8_t count = 0;
    for (const auto& n : s_saved) {
        if (count == wifi::MAX_SAVED || n.ssid.empty() || n.ssid.size() > 32) {
            continue;
        }
        buf[off] = (n.has_password ? 1 : 0) | (n.hidden ? 2 : 0);
        buf[off + 1] = (uint8_t)n.ssid.size();
        // noopengrep: mh.gitlab.c_buffer_rule-memcpy-CopyMemory - at most MAX_SAVED entries of 2 + 32 bytes (longer SSIDs skipped above), the size buf has
        memcpy(buf + off + 2, n.ssid.data(), n.ssid.size());
        off += 2 + n.ssid.size();
        count++;
    }
    buf[0] = 1;
    buf[1] = count;
    nvs_handle_t h;
    esp_err_t err = settings_nvs::open("wifi", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "nets", buf, off);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving networks failed: %s", esp_err_to_name(err));
    }
    return err;
}

static int find_saved(const std::string& ssid)
{
    for (size_t i = 0; i < s_saved.size(); i++) {
        if (s_saved[i].ssid == ssid) {
            return (int)i;
        }
    }
    return -1;
}

std::vector<wifi::Saved> wifi::saved()
{
    Lock lock;
    load_saved();
    return s_saved;
}

bool wifi::is_saved(const std::string& ssid)
{
    Lock lock;
    load_saved();
    return find_saved(ssid) >= 0;
}

esp_err_t wifi::remember(const std::string& ssid, const std::string& password, bool hidden)
{
    Lock lock;
    load_saved();
    Saved n;
    n.ssid = ssid;
    int existing = find_saved(ssid);
    n.hidden = hidden || (existing >= 0 && s_saved[existing].hidden);
    if (password.empty()) {
        vault::remove(secret_id(ssid));
    } else if (vault::put(secret_id(ssid), password) == ESP_OK) {
        n.has_password = true;
    } else {
        ESP_LOGW(TAG, "Password not saved (vault unavailable)");
    }
    if (existing >= 0) {
        s_saved.erase(s_saved.begin() + existing);
    }
    s_saved.insert(s_saved.begin(), n);
    while (s_saved.size() > MAX_SAVED) {
        vault::remove(secret_id(s_saved.back().ssid));   // Forget the least recently used
        s_saved.pop_back();
    }
    return store_saved();
}

void wifi::forget(const std::string& ssid)
{
    Lock lock;
    load_saved();
    int i = find_saved(ssid);
    if (i >= 0) {
        s_saved.erase(s_saved.begin() + i);
        store_saved();
    }
    vault::remove(secret_id(ssid));
}

// The network just used goes to the top of the list (its saved password stays as it is)
static void move_to_front(const std::string& ssid)
{
    Lock lock;
    load_saved();
    int i = find_saved(ssid);
    if (i > 0) {
        wifi::Saved n = s_saved[i];
        s_saved.erase(s_saved.begin() + i);
        s_saved.insert(s_saved.begin(), n);
        store_saved();
    }
}

bool wifi::saved_password(const std::string& ssid, std::string& password)
{
    return vault::get(secret_id(ssid), password) == ESP_OK;
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

static void set_link(bool up, const std::string& ssid, const std::string& ip)
{
    {
        Lock lock;
        s_ssid = up ? ssid : "";
        s_ip = up ? ip : "";
    }
    if (s_link_cb) {
        s_link_cb(up);
    }
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t* ev = (const wifi_event_sta_disconnected_t*)data;
        if (s_state == State::Connecting && ev && ev->ssid_len &&
            (ev->ssid_len != strnlen(s_target, sizeof(s_target)) || memcmp(ev->ssid, s_target, ev->ssid_len) != 0)) {
            return;   // A late event from the network left before this attempt
        }
        s_last_reason = ev ? ev->reason : 0;
        if (s_state == State::Connecting) {
            if (s_retries < CONNECT_RETRIES) {
                s_retries = s_retries + 1;
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_events, BIT_FAILED);
            }
        } else if (s_state == State::Connected) {
            ESP_LOGW(TAG, "Link lost (reason %u)", (unsigned)s_last_reason);
            s_state = State::Idle;
            set_link(false, "", "");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t* ev = (const ip_event_got_ip_t*)data;
        char ip[16];
        esp_ip4addr_ntoa(&ev->ip_info.ip, ip, sizeof(ip));
        wifi_config_t cfg = {};
        esp_wifi_get_config(WIFI_IF_STA, &cfg);
        std::string ssid((const char*)cfg.sta.ssid, strnlen((const char*)cfg.sta.ssid, sizeof(cfg.sta.ssid)));
        mbedtls_platform_zeroize(&cfg, sizeof(cfg));   // It holds the password too
        s_state = State::Connected;
        ESP_LOGI(TAG, "Connected, IP %s", ip);
        set_link(true, ssid, ip);
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}


// No modem sleep. The ESP32-S3's own WiFi (T-Deck) starts in WIFI_PS_MIN_MODEM, which sleeps
// the radio between beacons: a long-lived stream starves, drops and is skipped, and the
// handshake of a join can time out. The radio is always streaming, so it stays awake (on the
// Tab5 the request goes to the C6). While Bluetooth runs, coexistence needs modem sleep.
static void apply_power_save()
{
    if (esp_wifi_set_ps(s_power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi power save mode not changed");
    }
}

void wifi::set_power_save(bool on)
{
    s_power_save = on;
    if (s_started) {
        apply_power_save();
    }
}

esp_err_t wifi::start(progress_cb_t progress)
{
    if (s_started) {
        return ESP_OK;
    }
    if (!s_events) {
        s_events = xEventGroupCreate();
    }
    if (progress) {
        progress("Starting WiFi...");
    }
    // Tab5: the link to the WiFi co-processor; T-Deck: nothing
    esp_err_t err = board_wifi_prepare();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;   // Created already
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return err;
    }
    // Networks are saved by the radio (passwords in the vault): the driver (on the Tab5, the
    // co-processor) must not keep its own plain-text copy in flash
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    static bool handlers;   // Once, even if "Try again" runs start() after a failed start
    if (!handlers) {
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);
        handlers = true;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;
    apply_power_save();
    return ESP_OK;
}

bool wifi::started()
{
    return s_started;
}

void wifi::on_link(link_cb_t cb)
{
    s_link_cb = cb;
}

esp_err_t wifi::scan(std::vector<Network>& out)
{
    out.clear();
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        return err;
    }
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    count = std::min<uint16_t>(count, SCAN_MAX);
    std::vector<wifi_ap_record_t> records(count);
    if (count > 0) {
        err = esp_wifi_scan_get_ap_records(&count, records.data());
        if (err != ESP_OK) {
            return err;
        }
    } else {
        esp_wifi_clear_ap_list();
    }
    for (uint16_t i = 0; i < count; i++) {
        std::string ssid((const char*)records[i].ssid, strnlen((const char*)records[i].ssid, sizeof(records[i].ssid)));
        if (ssid.empty()) {
            continue;   // Hidden network
        }
        auto it = std::find_if(out.begin(), out.end(), [&](const Network& n) { return n.ssid == ssid; });
        if (it == out.end()) {
            Network n;
            n.ssid = ssid;
            n.rssi = records[i].rssi;
            n.open = records[i].authmode == WIFI_AUTH_OPEN || records[i].authmode == WIFI_AUTH_OWE;
            out.push_back(n);
        } else if (records[i].rssi > it->rssi) {
            it->rssi = records[i].rssi;
        }
    }
    std::sort(out.begin(), out.end(), [](const Network& a, const Network& b) { return a.rssi > b.rssi; });
    return ESP_OK;
}

static const char* reason_text(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return "Wrong password?";
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        case WIFI_REASON_BEACON_TIMEOUT:
            return "Network not found - out of range?";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return "The network uses old security (WEP/WPA) that the radio doesn't accept, "
                   "or no password was given for a secured network.";
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_ASSOC_TOOMANY:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_ASSOC_EXPIRE:
            return "The network refused the connection - try again.";
        default:
            return "The network didn't answer.";
    }
}

esp_err_t wifi::connect(const std::string& ssid, const std::string& password, std::string& error)
{
    error.clear();
    if (!s_started) {
        error = "WiFi isn't running.";
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid.empty() || ssid.size() > 32) {
        error = "A network name has 1 to 32 characters.";
        return ESP_ERR_INVALID_ARG;
    }
    if (!password.empty() && (password.size() < 8 || password.size() > 63)) {
        error = "WiFi passwords have 8 to 63 characters.";
        return ESP_ERR_INVALID_ARG;
    }

    // Leave the current network first (switching networks): not a lost link
    if (s_state != State::Idle) {
        s_state = State::Idle;
        esp_wifi_disconnect();
        set_link(false, "", "");
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // Lengths were checked above (SSID 1-32, password empty or 8-63); clamped to the fields anyway
    wifi_config_t cfg = {};
    std::copy_n(ssid.begin(), std::min(ssid.size(), sizeof(cfg.sta.ssid)), cfg.sta.ssid);
    std::copy_n(password.begin(), std::min(password.size(), sizeof(cfg.sta.password) - 1), cfg.sta.password);
    // A password means WPA2 or better: an impostor offering WEP/WPA1/open is refused.
    // WPA3 (SAE) networks are joined too; management frames are protected when the AP can.
    cfg.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    mbedtls_platform_zeroize(&cfg, sizeof(cfg));
    if (err != ESP_OK) {
        error = "WiFi configuration failed.";
        return err;
    }

    snprintf(s_target, sizeof(s_target), "%.*s", (int)ssid.size(), ssid.c_str());
    s_retries = 0;
    s_last_reason = 0;
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    s_state = State::Connecting;
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (bits & BIT_CONNECTED) {
        return ESP_OK;
    }
    s_state = State::Idle;
    esp_wifi_disconnect();
    error = (bits & BIT_FAILED) ? reason_text(s_last_reason) : "No answer from the network (timed out).";
    if (s_last_reason) {
        error += " (reason " + std::to_string((unsigned)s_last_reason) + ")";   // For a report
    }
    ESP_LOGW(TAG, "Connect failed (reason %u)", (unsigned)s_last_reason);
    return ESP_FAIL;
}

void wifi::disconnect()
{
    if (s_state != State::Idle) {
        s_state = State::Idle;
        esp_wifi_disconnect();
        set_link(false, "", "");
    }
}

bool wifi::connected()
{
    return s_state == State::Connected;
}

std::string wifi::ssid()
{
    Lock lock;
    return s_ssid;
}

std::string wifi::ip()
{
    Lock lock;
    return s_ip;
}

int wifi::rssi()
{
    wifi_ap_record_t ap;
    return connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : -100;
}

esp_err_t wifi::auto_connect(std::string& joined, std::string& error)
{
    joined.clear();
    error.clear();
    std::vector<Saved> saved_list = saved();
    if (saved_list.empty()) {
        return ESP_ERR_NOT_FOUND;
    }
    std::vector<Network> in_range;
    esp_err_t err = scan(in_range);
    if (err != ESP_OK) {
        error = "WiFi scan failed.";
        return err;
    }
    // Saved networks in range, strongest first; then hidden ones, which scans don't list
    std::vector<Network> candidates;
    for (const auto& n : in_range) {
        if (std::any_of(saved_list.begin(), saved_list.end(), [&](const Saved& s) { return s.ssid == n.ssid; })) {
            candidates.push_back(n);
        }
    }
    for (const auto& s : saved_list) {
        if (s.hidden && std::none_of(candidates.begin(), candidates.end(),
                                     [&](const Network& c) { return c.ssid == s.ssid; })) {
            Network n;
            n.ssid = s.ssid;
            n.open = !s.has_password;
            candidates.push_back(n);
        }
    }
    if (candidates.empty()) {
        return ESP_ERR_NOT_FOUND;
    }
    for (const auto& c : candidates) {
        auto s = std::find_if(saved_list.begin(), saved_list.end(), [&](const Saved& x) { return x.ssid == c.ssid; });
        std::string password;
        if (s->has_password && !saved_password(c.ssid, password)) {
            continue;   // Its password can't be read: the user picks it again
        }
        if (!s->has_password && !c.open) {
            continue;   // Secured, but saved without a password
        }
        err = connect(c.ssid, password, error);
        vault::wipe(password);
        if (err == ESP_OK) {
            joined = c.ssid;
            move_to_front(c.ssid);   // Most recently used first
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}
