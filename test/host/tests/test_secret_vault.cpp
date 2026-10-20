/*
 * vault: AES-256-GCM secrets in NVS, unlocked by the device secret; nothing readable in flash or logs.
 */

#include "secret_vault.hpp"
#include "helpers.hpp"

static const std::string SECRET = "correct horse battery staple";

static void fresh_vault()
{
    factory_device();
    CHECK(!vault::init());
    CHECK(vault::is_available());
}

TEST(vault_put_get_round_trip)
{
    fresh_vault();
    CHECK_EQ(vault::put("wifi:HomeNet", SECRET), ESP_OK);
    std::string out;
    CHECK_EQ(vault::get("wifi:HomeNet", out), ESP_OK);
    CHECK_EQ(out, SECRET);
    CHECK_EQ(fake::open_handles(), 0);
}

TEST(vault_missing_secret_not_found)
{
    fresh_vault();
    std::string out;
    CHECK_EQ(vault::get("profile:none", out), ESP_ERR_NOT_FOUND);
}

TEST(vault_remove_secret)
{
    fresh_vault();
    CHECK_EQ(vault::put("profile:1", SECRET), ESP_OK);
    CHECK_EQ(vault::remove("profile:1"), ESP_OK);
    std::string out;
    CHECK_EQ(vault::get("profile:1", out), ESP_ERR_NOT_FOUND);
}

TEST(vault_no_plaintext_in_flash)
{
    fresh_vault();
    CHECK_EQ(vault::put("wifi:HomeNet", SECRET), ESP_OK);
    std::string flash = fake::dump_all();
    CHECK(flash.find(SECRET) == std::string::npos);
    CHECK(flash.find("HomeNet") == std::string::npos);   // Ids are hashed into key names
}

TEST(vault_survives_reboot)
{
    fresh_vault();
    CHECK_EQ(vault::put("wifi:HomeNet", SECRET), ESP_OK);
    CHECK(!vault::init());            // Reboot: unlocks by itself
    CHECK(vault::is_available());
    std::string out;
    CHECK_EQ(vault::get("wifi:HomeNet", out), ESP_OK);
    CHECK_EQ(out, SECRET);
}

TEST(vault_rejects_oversized_secret)
{
    fresh_vault();
    CHECK_EQ(vault::put("profile:1", std::string(257, 'x')), ESP_ERR_INVALID_SIZE);
    CHECK_EQ(vault::put("profile:1", std::string(256, 'x')), ESP_OK);
}

TEST(vault_tampered_secret_fails_authentication)
{
    fresh_vault();
    CHECK_EQ(vault::put("profile:1", SECRET), ESP_OK);
    for (auto& kv : fake::store()["nvs"]["vault"]) {
        if (kv.first[0] == 's') {
            kv.second.bytes.back() ^= 0x01;
        }
    }
    std::string out;
    CHECK(vault::get("profile:1", out) != ESP_OK);
    CHECK(out.empty());
}

TEST(vault_secret_bound_to_its_id)
{
    fresh_vault();
    CHECK_EQ(vault::put("profile:a", SECRET), ESP_OK);
    CHECK_EQ(vault::put("profile:b", "other"), ESP_OK);
    // Move a's record under b's key name: the id is authenticated data, so it must not decrypt
    auto& ns = fake::store()["nvs"]["vault"];
    std::string a_key;
    std::string b_key;
    for (const auto& kv : ns) {
        if (kv.first[0] == 's' && kv.second.bytes.size() == 12 + 16 + SECRET.size()) {
            a_key = kv.first;
        } else if (kv.first[0] == 's') {
            b_key = kv.first;
        }
    }
    CHECK(!a_key.empty() && !b_key.empty());
    ns[b_key] = ns[a_key];
    std::string out;
    CHECK(vault::get("profile:b", out) != ESP_OK);
}

TEST(vault_reset_deletes_every_secret)
{
    fresh_vault();
    CHECK_EQ(vault::put("profile:1", SECRET), ESP_OK);
    CHECK_EQ(vault::reset(), ESP_OK);
    CHECK(vault::is_available());
    std::string out;
    CHECK_EQ(vault::get("profile:1", out), ESP_ERR_NOT_FOUND);
}

TEST(vault_secrets_never_logged)
{
    fresh_vault();
    CHECK_EQ(vault::put("wifi:HomeNet", SECRET), ESP_OK);
    vault::init();
    std::string out;
    vault::get("wifi:HomeNet", out);
    CHECK(!log_contains(SECRET));
    CHECK(!log_contains("HomeNet"));
}

TEST(vault_lost_device_secret_starts_over)
{
    // The wrapped key is useless without the device secret: the vault starts again, empty
    fresh_vault();
    CHECK_EQ(vault::put("wifi:HomeNet", SECRET), ESP_OK);
    fake::store()["nvs"].erase("vault_dev");
    CHECK(vault::init());
    CHECK(vault::is_available());
    std::string out;
    CHECK_EQ(vault::get("wifi:HomeNet", out), ESP_ERR_NOT_FOUND);
}

TEST(vault_unavailable_without_settings_partition)
{
    // No partition initialized: nothing can be saved, and the vault says so
    CHECK(!vault::init());
    CHECK(!vault::is_available());
    CHECK_EQ(vault::put("profile:1", SECRET), ESP_ERR_INVALID_STATE);
}

TEST(vault_wipe_clears_string)
{
    std::string s = SECRET;
    vault::wipe(s);
    CHECK(s.empty());
}
