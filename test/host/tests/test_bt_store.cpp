/*
 * Bluetooth devices (bt_store.cpp): paired devices kept in order and bounded, bonding keys
 * only in the vault (encrypted), Bluetooth off until turned on.
 */

#include "bt_store.hpp"
#include "secret_vault.hpp"
#include "helpers.hpp"

static void fresh()
{
    factory_device();
    CHECK(!vault::init());
}

static bt_store::Device device(uint8_t last, const std::string& name)
{
    bt_store::Device d;
    d.addr_type = 1;
    for (int i = 0; i < 6; i++) {
        d.addr[i] = (uint8_t)(0x10 + i);
    }
    d.addr[0] = last;
    d.name = name;
    return d;
}

TEST(bt_off_by_default_and_remembered)
{
    fresh();
    CHECK(!bt_store::enabled());
    CHECK_EQ(bt_store::set_enabled(true), ESP_OK);
    CHECK(bt_store::enabled());
    CHECK_EQ(bt_store::set_enabled(false), ESP_OK);
    CHECK(!bt_store::enabled());
}

TEST(bt_devices_most_recent_first_and_bounded)
{
    fresh();
    for (uint8_t i = 0; i < bt_store::MAX_DEVICES + 2; i++) {
        CHECK_EQ(bt_store::remember(device(i, "Keyboard " + std::to_string(i))), ESP_OK);
    }
    std::vector<bt_store::Device> list = bt_store::devices();
    CHECK_EQ(list.size(), bt_store::MAX_DEVICES);
    CHECK_EQ(list[0].name, std::string("Keyboard 5"));
    CHECK_EQ(list.back().name, std::string("Keyboard 2"));
    // Pairing again moves it to the front without a duplicate
    CHECK_EQ(bt_store::remember(device(3, "Keyboard 3")), ESP_OK);
    list = bt_store::devices();
    CHECK_EQ(list.size(), bt_store::MAX_DEVICES);
    CHECK_EQ(list[0].name, std::string("Keyboard 3"));
}

TEST(bt_long_names_cut)
{
    fresh();
    CHECK_EQ(bt_store::remember(device(1, std::string(60, 'x'))), ESP_OK);
    CHECK_EQ(bt_store::devices()[0].name.size(), bt_store::MAX_NAME);
}

TEST(bt_keys_only_in_the_vault)
{
    fresh();
    bt_store::Device d = device(7, "Remote");
    CHECK_EQ(bt_store::remember(d), ESP_OK);
    const std::string keys(150, '\x5A');
    CHECK_EQ(bt_store::put_keys(d.addr_type, d.addr, keys), ESP_OK);
    std::string out;
    CHECK(bt_store::get_keys(d.addr_type, d.addr, out));
    CHECK_EQ(out, keys);
    // Nowhere in the settings in the clear
    for (const auto& ns : fake::store()["nvs"]) {
        for (const auto& kv : ns.second) {
            std::string bytes(kv.second.bytes.begin(), kv.second.bytes.end());
            CHECK(bytes.find(std::string(16, '\x5A')) == std::string::npos);
        }
    }
}

TEST(bt_forget_removes_device_and_keys)
{
    fresh();
    bt_store::Device a = device(1, "A");
    bt_store::Device b = device(2, "B");
    CHECK_EQ(bt_store::remember(a), ESP_OK);
    CHECK_EQ(bt_store::remember(b), ESP_OK);
    CHECK_EQ(bt_store::put_keys(a.addr_type, a.addr, "keys-a"), ESP_OK);
    bt_store::forget(a.addr_type, a.addr);
    std::vector<bt_store::Device> list = bt_store::devices();
    CHECK_EQ(list.size(), (size_t)1);
    CHECK_EQ(list[0].name, std::string("B"));
    std::string out;
    CHECK(!bt_store::get_keys(a.addr_type, a.addr, out));
}

TEST(bt_damaged_record_keeps_what_is_readable)
{
    fresh();
    CHECK_EQ(bt_store::remember(device(1, "A")), ESP_OK);
    nvs_handle_t h;
    CHECK_EQ(settings_nvs::open("bt", NVS_READWRITE, &h), ESP_OK);
    const uint8_t bad[] = {1, 3, 1, 1, 2, 3, 4, 5, 6, 200};   // Says 3 devices; the name length is impossible
    CHECK_EQ(nvs_set_blob(h, "devs", bad, sizeof(bad)), ESP_OK);
    nvs_close(h);
    CHECK(bt_store::devices().empty());
    CHECK(log_contains("damaged"));
}
