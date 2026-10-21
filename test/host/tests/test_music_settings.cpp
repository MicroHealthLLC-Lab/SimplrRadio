/*
 * Music settings (music_library.cpp, NVS namespace "music"): defaults, what is kept across a
 * restart, no flash write when nothing changed, bounded favorites, and stored values that
 * don't make sense ignored.
 */

#include "music_library.hpp"
#include "helpers.hpp"

using namespace music;

TEST(music_settings_defaults)
{
    factory_device();
    Settings s = load_settings();
    CHECK_EQ(s.folder, std::string(""));
    CHECK(!s.shuffle);
    CHECK(s.repeat == Repeat::All);
    CHECK(s.online);
    CHECK(!s.music_last);
    CHECK_EQ(s.last_song, (uint32_t)0);
    CHECK(load_favorites().empty());
}

TEST(music_settings_kept)
{
    factory_device();
    Settings s;
    s.folder = "/Music";
    s.shuffle = true;
    s.repeat = Repeat::One;
    s.online = false;
    s.music_last = true;
    s.last_song = 0x12345678;
    CHECK_EQ(save_settings(s), ESP_OK);
    Settings back = load_settings();
    CHECK_EQ(back.folder, std::string("/Music"));
    CHECK(back.shuffle);
    CHECK(back.repeat == Repeat::One);
    CHECK(!back.online);
    CHECK(back.music_last);
    CHECK_EQ(back.last_song, (uint32_t)0x12345678);
}

TEST(music_settings_no_write_when_unchanged)
{
    factory_device();
    Settings s;
    s.folder = "/Music";
    CHECK_EQ(save_settings(s), ESP_OK);
    fake::fail_writes(true);
    CHECK_EQ(save_settings(s), ESP_OK);   // Nothing written, so nothing fails
    s.shuffle = true;
    CHECK(save_settings(s) != ESP_OK);    // A change is written
    fake::fail_writes(false);
}

TEST(music_settings_bad_values_ignored)
{
    factory_device();
    nvs_handle_t h;
    CHECK_EQ(settings_nvs::open("music", NVS_READWRITE, &h), ESP_OK);
    CHECK_EQ(nvs_set_str(h, "dir", "relative/path"), ESP_OK);
    CHECK_EQ(nvs_set_u8(h, "repeat", 9), ESP_OK);
    nvs_close(h);
    Settings s = load_settings();
    CHECK_EQ(s.folder, std::string(""));
    CHECK(s.repeat == Repeat::All);
    CHECK_EQ(settings_nvs::open("music", NVS_READWRITE, &h), ESP_OK);
    CHECK_EQ(nvs_set_str(h, "dir", "/Music/../.."), ESP_OK);
    nvs_close(h);
    CHECK_EQ(load_settings().folder, std::string(""));
}

TEST(music_favorites_kept_and_bounded)
{
    factory_device();
    std::vector<uint32_t> favs = {1, 2, 3};
    CHECK_EQ(save_favorites(favs), ESP_OK);
    CHECK_EQ(load_favorites().size(), (size_t)3);
    std::vector<uint32_t> many;
    for (uint32_t i = 1; i <= (uint32_t)MAX_FAVORITES + 50; i++) {
        many.push_back(i);
    }
    CHECK_EQ(save_favorites(many), ESP_OK);
    CHECK_EQ(load_favorites().size(), (size_t)MAX_FAVORITES);
    CHECK_EQ(save_favorites({}), ESP_OK);
    CHECK(load_favorites().empty());
    CHECK_EQ(save_favorites({}), ESP_OK);   // Erasing nothing is fine
}
