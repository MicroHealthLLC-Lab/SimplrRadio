/*
 * Radio settings (settings.cpp): station search defaults and choices, volume and last
 * station, with no flash write when nothing changed.
 */

#include "settings.h"
#include "helpers.hpp"
#include <cstring>

TEST(settings_search_defaults)
{
    factory_device();
    station_search_t s;
    settings_get_search(&s);
    CHECK_EQ(std::string(s.country), std::string("US"));
    CHECK_EQ(std::string(s.state), std::string("Texas"));
    CHECK_EQ(s.count, SEARCH_COUNT_MAX);
}

TEST(settings_search_saved_including_empty_choices)
{
    factory_device();
    station_search_t s = {};
    snprintf(s.country, sizeof(s.country), "%s", "");
    snprintf(s.state, sizeof(s.state), "%s", "");
    s.count = 12;
    CHECK_EQ(settings_set_search(&s), ESP_OK);
    station_search_t out;
    settings_get_search(&out);
    CHECK_EQ(std::string(out.country), std::string(""));   // Worldwide stays worldwide
    CHECK_EQ(std::string(out.state), std::string(""));
    CHECK_EQ(out.count, 12);
}

TEST(settings_search_count_out_of_range_ignored)
{
    factory_device();
    nvs_handle_t h;
    CHECK_EQ(settings_nvs::open("radio", NVS_READWRITE, &h), ESP_OK);
    CHECK_EQ(nvs_set_u8(h, "count", 200), ESP_OK);
    nvs_close(h);
    station_search_t out;
    settings_get_search(&out);
    CHECK_EQ(out.count, SEARCH_COUNT_MAX);
}

TEST(settings_volume_round_trip_and_bounds)
{
    factory_device();
    CHECK_EQ(settings_get_volume(60), 60);
    CHECK_EQ(settings_set_volume(35), ESP_OK);
    CHECK_EQ(settings_get_volume(60), 35);
    nvs_handle_t h;
    CHECK_EQ(settings_nvs::open("radio", NVS_READWRITE, &h), ESP_OK);
    CHECK_EQ(nvs_set_u8(h, "vol", 250), ESP_OK);
    nvs_close(h);
    CHECK_EQ(settings_get_volume(60), 60);   // Garbage: the fallback
}

TEST(settings_unchanged_values_not_rewritten)
{
    factory_device();
    CHECK_EQ(settings_set_volume(40), ESP_OK);
    CHECK_EQ(settings_set_station("http://example.com/stream"), ESP_OK);
    fake::fail_writes(true);   // Any write would now fail
    CHECK_EQ(settings_set_volume(40), ESP_OK);
    CHECK_EQ(settings_set_station("http://example.com/stream"), ESP_OK);
    CHECK(settings_set_volume(41) != ESP_OK);
    fake::fail_writes(false);
    char url[300];
    CHECK(settings_get_station(url, sizeof(url)));
    CHECK_EQ(std::string(url), std::string("http://example.com/stream"));
}

TEST(settings_without_partition)
{
    // No settings partition: defaults, and saving reports the failure
    station_search_t s;
    settings_get_search(&s);
    CHECK_EQ(std::string(s.country), std::string("US"));
    CHECK(settings_set_volume(10) != ESP_OK);
    char url[16];
    CHECK(!settings_get_station(url, sizeof(url)));
}
