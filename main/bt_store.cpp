/*
 * Bluetooth Devices Implementation (bt_store.hpp)
 */

#include "bt_store.hpp"
#include "secret_vault.hpp"
#include "settings_nvs.hpp"
#include "esp_log.h"
#include <algorithm>
#include <cstdio>

static const char *TAG = "bt_store";
static const char *NS = "bt";

static const size_t RECORD_MAX = 2 + bt_store::MAX_DEVICES * (1 + 6 + 1 + bt_store::MAX_NAME);

static std::string secret_id(uint8_t addr_type, const uint8_t addr[6])
{
    char id[3 + 1 + 12 + 1];
    snprintf(id, sizeof(id), "bt:%u%02x%02x%02x%02x%02x%02x", (unsigned)(addr_type & 1), addr[5], addr[4], addr[3],
             addr[2], addr[1], addr[0]);
    return id;
}

bool bt_store::same(const Device& a, uint8_t addr_type, const uint8_t addr[6])
{
    return a.addr_type == addr_type && std::equal(a.addr, a.addr + 6, addr);
}

bool bt_store::enabled()
{
    nvs_handle_t h;
    if (settings_nvs::open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t on = 0;
    bool result = nvs_get_u8(h, "on", &on) == ESP_OK && on == 1;
    nvs_close(h);
    return result;
}

esp_err_t bt_store::set_enabled(bool on)
{
    nvs_handle_t h;
    esp_err_t err = settings_nvs::open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "on", on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

std::vector<bt_store::Device> bt_store::devices()
{
    std::vector<Device> out;
    nvs_handle_t h;
    if (settings_nvs::open(NS, NVS_READONLY, &h) != ESP_OK) {
        return out;
    }
    uint8_t buf[RECORD_MAX];
    size_t len = sizeof(buf);
    if (nvs_get_blob(h, "devs", buf, &len) == ESP_OK && len >= 2 && buf[0] == 1) {
        size_t off = 2;
        for (int i = 0; i < buf[1] && out.size() < MAX_DEVICES; i++) {
            if (off + 8 > len || buf[off + 7] > MAX_NAME || off + 8 + buf[off + 7] > len) {
                ESP_LOGW(TAG, "Paired devices record damaged; keeping %u", (unsigned)out.size());
                break;
            }
            Device d;
            d.addr_type = buf[off] & 1;
            std::copy(buf + off + 1, buf + off + 7, d.addr);
            d.name.assign((const char*)buf + off + 8, buf[off + 7]);
            out.push_back(d);
            off += 8 + buf[off + 7];
        }
    }
    nvs_close(h);
    return out;
}

static esp_err_t store(const std::vector<bt_store::Device>& list)
{
    uint8_t buf[RECORD_MAX];
    size_t off = 2;
    uint8_t count = 0;
    for (const auto& d : list) {
        if (count == bt_store::MAX_DEVICES) {
            break;
        }
        size_t name_len = std::min(d.name.size(), bt_store::MAX_NAME);
        buf[off] = d.addr_type & 1;
        std::copy(d.addr, d.addr + 6, buf + off + 1);
        buf[off + 7] = (uint8_t)name_len;
        std::copy(d.name.begin(), d.name.begin() + (long)name_len, buf + off + 8);
        off += 8 + name_len;
        count++;
    }
    buf[0] = 1;
    buf[1] = count;
    nvs_handle_t h;
    esp_err_t err = settings_nvs::open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "devs", buf, off);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving paired devices failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t bt_store::remember(const Device& device)
{
    std::vector<Device> list = devices();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const Device& d) { return same(d, device.addr_type, device.addr); }),
               list.end());
    Device d = device;
    d.addr_type &= 1;
    if (d.name.size() > MAX_NAME) {
        d.name.resize(MAX_NAME);
    }
    list.insert(list.begin(), d);
    while (list.size() > MAX_DEVICES) {
        remove_keys(list.back().addr_type, list.back().addr);   // The oldest goes
        list.pop_back();
    }
    return store(list);
}

void bt_store::forget(uint8_t addr_type, const uint8_t addr[6])
{
    std::vector<Device> list = devices();
    size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Device& d) { return same(d, addr_type, addr); }),
               list.end());
    if (list.size() != before) {
        store(list);
    }
    remove_keys(addr_type, addr);
}

esp_err_t bt_store::put_keys(uint8_t addr_type, const uint8_t addr[6], const std::string& keys)
{
    return vault::put(secret_id(addr_type, addr), keys);
}

bool bt_store::get_keys(uint8_t addr_type, const uint8_t addr[6], std::string& keys)
{
    return vault::get(secret_id(addr_type, addr), keys) == ESP_OK;
}

void bt_store::remove_keys(uint8_t addr_type, const uint8_t addr[6])
{
    vault::remove(secret_id(addr_type, addr));
}
