/*
 * SimplrRadio app: boot, screens and input.
 *
 * Tasks
 *   app task (app_main)  owns every screen and the player. Waits on one event queue (keys,
 *                        touches handed over from LVGL, finished jobs, WiFi link changes),
 *                        polls the keyboard, ticks the player. Holds the display lock
 *                        while it changes the screen, never while it waits.
 *   worker               runs one blocking job at a time (WiFi start / scan / connect, the
 *                        station directory, sign-in pages) and posts its result back, so the
 *                        screen stays responsive. A job's result is dropped if the user left
 *                        the screen that asked for it (generation counter).
 *   LVGL (esp_lvgl_port) draws; its callbacks only post events.
 *
 * Screens follow SimplrSSH's menus: numbered rows (type the number, use Up/Down + Enter, the
 * T-Deck's trackball, or tap), Esc / Left / the title bar's arrow goes back one level (T-Deck:
 * hold the trackball), Ctrl+Del (Tab5) returns to Now Playing. Questions are answered in a
 * text field with the hardware keyboard or the on-screen one.
 */

#include "board.h"
#include "keys.h"
#include "player.h"
#include "portal_client.hpp"
#include "radio_browser.h"
#include "secret_vault.hpp"
#include "settings.h"
#include "settings_nvs.hpp"
#include "ui_view.hpp"
#include "wifi_mgr.hpp"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

static const char *TAG = "app";

// ---------------------------------------------------------------------------
// Events and the worker
// ---------------------------------------------------------------------------

enum class Ev : uint8_t {
    Tap,        // arg: row
    Back,
    Submit,     // On-screen keyboard OK
    Button,     // arg: NpButton
    Swipe,      // arg: lv_dir_t
    JobDone,    // ptr: Job
    Link,       // arg: 1 up, 0 down
    Status,     // ptr: malloc'd text for the boot screen
};

struct Event {
    Ev type;
    int32_t arg;
    void* ptr;
};

struct Job {
    std::function<void()> run;    // Worker task, no display lock
    std::function<void()> done;   // App task, display lock held
};

static QueueHandle_t s_events;
static QueueHandle_t s_jobs;

static void post(Ev type, int32_t arg = 0, void* ptr = NULL)
{
    Event ev = {type, arg, ptr};
    if (xQueueSend(s_events, &ev, 0) != pdTRUE && ptr && type == Ev::Status) {
        free(ptr);
    }
}

static void worker_task(void*)
{
    for (;;) {
        Job* job = NULL;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE && job) {
            job->run();
            Event ev = {Ev::JobDone, 0, job};
            xQueueSend(s_events, &ev, portMAX_DELAY);   // Never dropped: it owns the job
        }
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class Screen {
    Boot,            // Starting / boot errors
    NowPlaying,
    Menu,
    Stations,
    Wifi,
    WifiScan,
    WifiSaved,
    WifiForget,
    WifiForgetConfirm,
    WifiHidden,      // Prompt: SSID
    WifiPassword,    // Prompt
    Busy,            // A job is running (connecting, loading...)
    Search,
    SearchCountry,   // Prompts
    SearchState,
    SearchCount,
    About,
    PortalPage,
    PortalField,     // Prompt
    PortalOption,
};

static UiView s_ui;
static Screen s_screen = Screen::Boot;
static ui::Page s_page;                 // What the screen shows (menus, prompts)
static ui::Notice s_notice;             // Results and errors, shown on the next page
static std::string s_typed;             // Row number being typed
static int64_t s_typed_ms;
static uint32_t s_generation;           // Bumped when a screen is left: stale job results are dropped
static bool s_busy;                     // A job is queued or running
static std::string s_busy_text;
static std::string s_boot_status = "Starting...";
static bool s_boot_failed;
static bool s_booted;                   // Got past WiFi start

// WiFi screens
static std::vector<wifi::Network> s_scan;
static bool s_scanned;
static std::string s_ssid;              // Network being joined / forgotten
static bool s_hidden;
static bool s_link_up;
static int64_t s_reconnect_at;          // Background reconnect (0 = not scheduled)
static bool s_user_offline;             // The user chose Disconnect: no background reconnect
static int s_rssi = -100;               // Signal, refreshed every few seconds (an RPC to the C6)
static int64_t s_rssi_at;
static int s_reconnect_delay_s;

// Stations
static bool s_loading_stations;
static int64_t s_reload_at;             // Retry of a failed station list load (0 = none)
static int s_reload_delay_s = 5;
static std::string s_np_typed;          // Station number typed on Now Playing

// Sign-in page
static portal::Page s_portal;
static portal::Cookies s_cookies;
static int s_portal_field = -1;
static int s_portal_selected = -1;

// Last status shown in the title bar
static uint32_t s_np_version = UINT32_MAX;

static int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

static void defer(std::function<void()> fn);
static void go(Screen screen);
static void render();
static void load_stations();
static void after_connect(bool quiet);
static void scan_networks();

// Queues a blocking job; `done` runs on the app task unless the user moved on meanwhile
static bool run_job(std::function<void()> run, std::function<void()> done)
{
    if (s_busy) {
        s_notice.add("Still working on the last request - one moment.\n");
        return false;
    }
    Job* job = new Job{run, done};
    s_busy = true;
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        delete job;
        s_busy = false;
        return false;
    }
    return true;
}

// The user leaves a screen: results still on their way for it are dropped
static void new_generation()
{
    s_generation++;
}

// ---------------------------------------------------------------------------
// Player actions that wait for tasks run after the display lock is released
// ---------------------------------------------------------------------------

static std::vector<std::function<void()>> s_deferred;

static void defer(std::function<void()> fn)
{
    s_deferred.push_back(fn);
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

// "Enter = connect   Esc = back   Tab = show", in the board's key names
static std::string keys_hint(const char* enter, bool reveal)
{
    const board_ui_t* ui = board_ui();
    std::string hint = std::string("Enter = ") + enter + "   " + ui->key_back + " = back";
    if (reveal) {
        hint += ui->key_reveal ? std::string("   ") + ui->key_reveal + " = show" : "   tap Show to see it";
    }
    return hint;
}

static std::string shown(const std::string& ssid)
{
    return portal::printable(ssid, 32);
}

static std::string shorten(const std::string& s, size_t max)
{
    return s.size() <= max ? s : s.substr(0, max - 3) + "...";
}

static bool is_number(const std::string& s)
{
    return !s.empty() && s.size() <= 3 && std::all_of(s.begin(), s.end(), ::isdigit);
}

// A typed number (up to three digits, is_number()), or -1
static int number_value(const std::string& s)
{
    if (!is_number(s)) {
        return -1;
    }
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    return end && *end == '\0' && v >= 0 && v <= 999 ? (int)v : -1;
}

static std::string search_summary()
{
    station_search_t search;
    settings_get_search(&search);
    std::string where = search.country[0] ? std::string(search.country) : "Worldwide";
    if (search.country[0] && search.state[0]) {
        where += ", " + std::string(search.state);
    }
    return where + " - " + std::to_string(search.count) + " stations";
}

// ---------------------------------------------------------------------------
// Title bar and Now Playing
// ---------------------------------------------------------------------------

static void update_status()
{
    if (s_link_up && now_ms() - s_rssi_at > 10000) {
        s_rssi = wifi::rssi();
        s_rssi_at = now_ms();
    }
    s_ui.set_status(s_link_up, s_link_up ? s_rssi : -100, player_volume(), player_muted());
}

static void refresh_now_playing()
{
    NowPlaying np;
    int index = player_index();
    const radio_station_t* st = player_station(index);
    if (st) {
        np.station = st->name;
        np.info = std::to_string(index + 1) + " / " + std::to_string(player_count());
        if (st->bitrate) {
            np.info += "   " + std::to_string(st->bitrate) + " kbps";
        }
        if (st->tags[0]) {
            np.info += "   " + shorten(st->tags, 48);
        }
        char title[256];
        player_title(title, sizeof(title));
        np.song = title;
    }
    np.muted = player_muted();
    if (!s_np_typed.empty()) {
        np.status = "Go to station " + s_np_typed + "_   (Enter)";
    } else if (!s_link_up && s_booted) {
        np.status = "WiFi is off - reconnecting...";
        np.status_warn = true;
    } else if (s_loading_stations) {
        np.status = "Loading stations...";
    } else if (player_count() == 0) {
        np.status = s_reload_at ? "The station list couldn't load - trying again soon." : "No stations found.";
        np.status_warn = true;
    } else {
        switch (player_status()) {
            case PLAYER_CONNECTING: np.status = "Connecting..."; break;
            case PLAYER_UNAVAILABLE:
                np.status = "This station is unavailable right now - try the next one.";
                np.status_warn = true;
                break;
            case PLAYER_IDLE: np.status = "Stopped"; break;
            case PLAYER_PLAYING: np.status = np.muted ? "Muted" : ""; break;
        }
    }
    s_ui.update_now_playing(np);
    update_status();
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

static std::string signal_detail(const wifi::Network& n)
{
    return std::string(ui::signal_words(n.rssi)) + (n.open ? ", open" : ", secured");
}

static void build_portal_page(ui::Page& p);

// The page for the current screen
static void build_page(ui::Page& p)
{
    p = ui::Page();
    switch (s_screen) {
        case Screen::Boot:
            p.title = "SimplrRadio";
            p.info = s_boot_status;
            p.busy = !s_boot_failed;
            if (s_boot_failed) {
                p.rows = {ui::row("1", "Try again", LV_SYMBOL_REFRESH), ui::row("2", "Restart", LV_SYMBOL_POWER)};
            }
            break;

        case Screen::Busy:
            p.title = "Please wait";
            p.info = s_busy_text;
            p.busy = true;
            break;

        case Screen::Menu: {
            p.title = "Menu";
            bool up = s_link_up;
            p.rows = {
                ui::row("1", "Stations", LV_SYMBOL_LIST,
                        player_count() ? std::to_string(player_count()) + " stations" : "none yet", ui::Mark::More),
                ui::row("2", "WiFi", LV_SYMBOL_WIFI, up ? shown(wifi::ssid()) : "not connected", ui::Mark::More),
                ui::row("3", "Station search", LV_SYMBOL_DIRECTORY, search_summary(), ui::Mark::More),
                ui::row("4", "Sign in to this network", LV_SYMBOL_RIGHT, up ? "cafe / hotel page" : "not connected",
                        ui::Mark::More),
                ui::row("5", "About", LV_SYMBOL_HOME, "", ui::Mark::More),
                ui::back_row("Now Playing"),
            };
            break;
        }

        case Screen::Stations: {
            p.title = "Stations";
            int count = player_count();
            p.info = count ? search_summary() : "No stations loaded yet.";
            for (int i = 0; i < count; i++) {
                const radio_station_t* st = player_station(i);
                std::string detail = st->bitrate ? std::to_string(st->bitrate) + " kbps" : "";
                if (st->tags[0]) {
                    detail += (detail.empty() ? "" : "  ") + shorten(st->tags, 32);
                }
                p.rows.push_back(ui::row(std::to_string(i + 1), st->name, NULL, detail,
                                         i == player_index() ? ui::Mark::Chosen : ui::Mark::None));
            }
            p.rows.push_back(ui::row("r", "Load the list again", LV_SYMBOL_REFRESH));
            p.rows.push_back(ui::back_row());
            p.selected = player_index() >= 0 ? player_index() : 0;
            break;
        }

        case Screen::Wifi: {
            p.title = "WiFi";
            if (s_link_up) {
                p.info = "Connected to " + shown(wifi::ssid()) + " (" + ui::signal_words(wifi::rssi()) + "), " +
                         wifi::ip();
            } else {
                p.info = "Not connected";
            }
            size_t saved = wifi::saved().size();
            p.rows = {
                ui::row("1", "Scan and connect", LV_SYMBOL_WIFI, "", ui::Mark::More),
                ui::row("2", "Saved networks", LV_SYMBOL_LIST, saved ? std::to_string(saved) : "none", ui::Mark::More),
                ui::row("3", "Forget a network", LV_SYMBOL_TRASH),
                ui::row("4", "Disconnect", LV_SYMBOL_CLOSE, s_link_up ? "" : "not connected"),
                ui::row("5", "Sign in (cafe / hotel page)", LV_SYMBOL_RIGHT, "", ui::Mark::More),
                ui::back_row(),
            };
            p.rows[2].tone = ui::Tone::Danger;
            break;
        }

        case Screen::WifiScan: {
            p.title = "Networks";
            if (!s_scanned) {
                p.info = "Scanning...";
                p.busy = true;
                break;
            }
            p.info = s_scan.empty() ? "No networks found." : "Strongest first. Pick yours:";
            for (size_t i = 0; i < s_scan.size(); i++) {
                const wifi::Network& n = s_scan[i];
                bool here = s_link_up && n.ssid == wifi::ssid();
                std::string detail = signal_detail(n);
                if (here) {
                    detail += ", connected";
                } else if (wifi::is_saved(n.ssid)) {
                    detail += ", saved";
                }
                p.rows.push_back(ui::row(std::to_string(i + 1), shown(n.ssid), n.open ? LV_SYMBOL_WIFI : LV_SYMBOL_EYE_CLOSE,
                                         detail, here ? ui::Mark::Chosen : ui::Mark::None));
            }
            size_t n = s_scan.size();
            p.rows.push_back(ui::row(std::to_string(n + 1), "Hidden network...", LV_SYMBOL_EDIT));
            p.rows.push_back(ui::row(std::to_string(n + 2), "Scan again", LV_SYMBOL_REFRESH));
            p.rows.push_back(ui::back_row());
            break;
        }

        case Screen::WifiSaved:
        case Screen::WifiForget: {
            bool forget = s_screen == Screen::WifiForget;
            p.title = forget ? "Forget a network" : "Saved networks";
            std::vector<wifi::Saved> saved = wifi::saved();
            p.info = saved.empty() ? "No saved networks. Use 'Scan and connect' to add one."
                                   : (forget ? "Pick the network to forget:" : "Most recently used first:");
            for (size_t i = 0; i < saved.size(); i++) {
                bool here = s_link_up && saved[i].ssid == wifi::ssid();
                std::string detail = saved[i].has_password ? "password saved" : "open";
                if (saved[i].hidden) {
                    detail += ", hidden";
                }
                if (here) {
                    detail = "connected, " + detail;
                }
                p.rows.push_back(ui::row(std::to_string(i + 1), shown(saved[i].ssid),
                                         forget ? LV_SYMBOL_TRASH : LV_SYMBOL_WIFI, detail));
                if (forget) {
                    p.rows.back().tone = ui::Tone::Danger;
                }
            }
            p.rows.push_back(ui::back_row());
            break;
        }

        case Screen::WifiForgetConfirm:
            p.title = "Forget network";
            p.info = "Forget " + shown(s_ssid) + " and its saved password?";
            p.rows = {ui::row("y", "Forget it", LV_SYMBOL_TRASH), ui::row("n", "Keep it", LV_SYMBOL_CLOSE)};
            p.rows[0].tone = ui::Tone::Danger;
            p.selected = 1;   // Default: keep
            break;

        case Screen::WifiHidden:
            p.title = "Hidden network";
            p.question = "Network name (SSID)";
            p.hint = keys_hint("next", false);
            p.max_len = 32;
            break;

        case Screen::WifiPassword:
            p.title = shown(s_ssid);
            p.question = "Password for " + shown(s_ssid);
            p.secret = true;
            p.max_len = 63;
            p.hint = (s_hidden ? "Leave it empty for an open network.   " : "") + keys_hint("connect", true);
            break;

        case Screen::Search: {
            station_search_t search;
            settings_get_search(&search);
            p.title = "Station search";
            p.info = "Which stations to load from the Radio Browser directory (most popular first).";
            p.rows = {
                ui::row("1", "Country", LV_SYMBOL_GPS, search.country[0] ? search.country : "worldwide", ui::Mark::More),
                ui::row("2", "State / region", LV_SYMBOL_HOME, search.state[0] ? search.state : "all", ui::Mark::More),
                ui::row("3", "Number of stations", LV_SYMBOL_LIST, std::to_string(search.count), ui::Mark::More),
                ui::row("4", "Load stations now", LV_SYMBOL_REFRESH),
                ui::back_row(),
            };
            break;
        }

        case Screen::SearchCountry: {
            station_search_t search;
            settings_get_search(&search);
            p.title = "Country";
            p.question = "Country code";
            p.hint = "Two letters, e.g. US, GB, DE, FR. Leave it empty for worldwide.";
            p.value = search.country;
            p.max_len = 2;
            break;
        }

        case Screen::SearchState: {
            station_search_t search;
            settings_get_search(&search);
            p.title = "State / region";
            p.question = "State or region";
            p.hint = "As the directory spells it, e.g. Texas, Bavaria. Leave it empty for the whole country.";
            p.value = search.state;
            p.max_len = SEARCH_STATE_LEN - 1;
            break;
        }

        case Screen::SearchCount: {
            station_search_t search;
            settings_get_search(&search);
            p.title = "Number of stations";
            p.question = "How many stations (1-" + std::to_string(SEARCH_COUNT_MAX) + ")";
            p.value = std::to_string(search.count);
            p.max_len = 2;
            break;
        }

        case Screen::About: {
            const esp_app_desc_t* app = esp_app_get_description();
            p.title = "About";
            p.info = std::string("SimplrRadio ") + app->version + " on " + board_name() + "\n";
            if (s_link_up) {
                p.info += "WiFi: " + shown(wifi::ssid()) + " (" + ui::signal_words(wifi::rssi()) + "), IP " +
                          wifi::ip() + "\n";
            } else {
                p.info += "WiFi: not connected\n";
            }
            if (board_wifi_firmware()) {
                p.info += std::string("WiFi chip firmware: ") + board_wifi_firmware() + "\n";
            }
            p.info += "Stations: " + std::to_string(player_count()) + " (" + search_summary() + ")\n";
            p.info += "Free memory: " + std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024) +
                      " KB internal, " + std::to_string(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024) + " KB PSRAM\n";
            p.info += std::string("Settings: ") + settings_nvs::partition() + " partition, WiFi passwords encrypted\n";
            p.info += "Stations from radio-browser.info";
            p.rows = {ui::back_row()};
            break;
        }

        case Screen::PortalPage:
        case Screen::PortalField:
        case Screen::PortalOption:
            build_portal_page(p);
            break;

        case Screen::NowPlaying:
            break;
    }
}

// Shows the current screen
static bool portal_field_valid()
{
    return s_portal_field >= 0 && (size_t)s_portal_field < s_portal.fields.size();
}

static void render()
{
    if ((s_screen == Screen::PortalField || s_screen == Screen::PortalOption) && !portal_field_valid()) {
        s_screen = Screen::PortalPage;   // The field went away (page reloaded)
    }
    if (s_screen == Screen::NowPlaying) {
        s_ui.show_now_playing();
        s_ui.set_title(s_link_up ? "SimplrRadio" : "SimplrRadio - offline");
        refresh_now_playing();
        return;
    }
    build_page(s_page);
    int selected = ui::clamp_selection(s_page, s_page.selected);
    s_ui.show_page(s_page, s_notice.text(), selected);
    s_ui.set_back(s_screen != Screen::Boot && s_screen != Screen::Busy);
    if (s_ui.prompt_shown()) {
        s_ui.show_keyboard(!board_keyboard_present());   // On-screen keyboard for touch-only use
    }
    s_notice.clear();
    s_typed.clear();
    update_status();
}

static void go(Screen screen)
{
    if (screen != s_screen) {
        new_generation();
    }
    s_screen = screen;
    render();
}

// A background result arrived: show it without rebuilding a prompt the user is typing in
static void refresh()
{
    if (s_ui.prompt_shown()) {
        s_ui.set_notice(s_notice.text());
    } else {
        render();
    }
}

static void busy(const std::string& text)
{
    s_busy_text = text;
    s_screen = Screen::Busy;
    render();
}

// ---------------------------------------------------------------------------
// Stations
// ---------------------------------------------------------------------------

static void load_stations()
{
    if (s_loading_stations) {
        return;
    }
    auto* list = (radio_station_t*)heap_caps_calloc(MAX_STATIONS, sizeof(radio_station_t), MALLOC_CAP_SPIRAM);
    if (!list) {
        s_notice.add("Not enough memory to load stations.\n");
        return;
    }
    auto* count = new int(0);
    bool started = run_job(
        [list, count]() { *count = radio_browser_fetch(list, MAX_STATIONS); },
        [list, count]() {
            s_loading_stations = false;
            int n = *count;
            if (n > 0) {
                s_reload_at = 0;
                s_reload_delay_s = 5;
                defer([list, n]() {
                    player_set_stations(list, n);
                    heap_caps_free(list);
                });
                if (s_screen == Screen::Boot || s_screen == Screen::Busy) {
                    s_screen = Screen::NowPlaying;
                }
                ESP_LOGI(TAG, "%d stations", n);
            } else {
                heap_caps_free(list);
                // The directory is flaky at times: retry with backoff (5 s up to 2 min)
                s_reload_at = now_ms() + s_reload_delay_s * 1000;
                s_reload_delay_s = std::min(s_reload_delay_s * 2, 120);
                s_notice.add("The station list couldn't be loaded (no internet, or the directory is busy). "
                             "Trying again soon.\n");
                if (s_screen == Screen::Boot || s_screen == Screen::Busy) {
                    s_screen = Screen::NowPlaying;
                }
            }
            delete count;
            refresh();
        });
    if (started) {
        s_loading_stations = true;
        s_reload_at = 0;
    } else {
        heap_caps_free(list);
        delete count;
    }
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

static void connected_ok(const std::string& ssid)
{
    s_notice.add(("Connected to " + shown(ssid) + ".\n").c_str());
    s_reconnect_at = 0;
    s_reconnect_delay_s = 0;
    after_connect(false);
}

// Joins `ssid` with `password`; saves it when it works
static void connect_to(const std::string& ssid, const std::string& password, bool hidden)
{
    auto* err = new esp_err_t(ESP_FAIL);
    auto* error = new std::string();
    auto* pass = new std::string(password);
    std::string name = ssid;
    bool open = password.empty() && !hidden;
    uint32_t gen = s_generation;   // busy() below keeps it: Esc out of "Connecting..." changes it
    bool started = run_job(
        [=]() {
            *err = wifi::connect(name, *pass, *error);
            if (*err == ESP_OK) {
                wifi::remember(name, *pass, hidden);   // Every network that connects is saved, newest first
            }
            vault::wipe(*pass);
        },
        [=]() {
            bool waiting = gen == s_generation && s_screen == Screen::Busy;   // The user is still here
            if (*err == ESP_OK) {
                if (waiting) {
                    connected_ok(name);
                } else {
                    s_notice.add(("Connected to " + shown(name) + ".\n").c_str());
                    after_connect(true);
                    refresh();
                }
            } else {
                s_notice.add(("Couldn't join " + shown(name) + ": " + *error + "\n").c_str());
                if (!waiting) {
                    refresh();
                } else if (open) {
                    go(s_scanned ? Screen::WifiScan : Screen::Wifi);   // No password to ask for
                } else {
                    s_ssid = name;
                    s_hidden = hidden;
                    go(Screen::WifiPassword);   // Usually the password: ask again
                }
            }
            delete err;
            delete error;
            delete pass;
        });
    if (started) {
        busy("Connecting to " + shown(ssid) + "...");
    } else {
        vault::wipe(*pass);
        delete err;
        delete error;
        delete pass;
        render();
    }
}

// A network was picked (scan list, saved list or typed SSID)
static void choose_network(const std::string& ssid, bool open, bool hidden)
{
    s_ssid = ssid;
    s_hidden = hidden;
    std::string password;
    if (wifi::is_saved(ssid) && wifi::saved_password(ssid, password)) {
        connect_to(ssid, password, hidden);
        vault::wipe(password);
    } else if (open) {
        connect_to(ssid, "", hidden);
    } else {
        go(Screen::WifiPassword);
    }
}

static void scan_networks()
{
    s_scanned = false;
    s_scan.clear();
    go(Screen::WifiScan);
    auto* list = new std::vector<wifi::Network>();
    auto* err = new esp_err_t(ESP_FAIL);
    uint32_t gen = s_generation;
    bool started = run_job([=]() { *err = wifi::scan(*list); },
                           [=]() {
                               if (gen == s_generation && s_screen == Screen::WifiScan) {
                                   if (*err != ESP_OK) {
                                       s_notice.add("WiFi scan failed - try again.\n");
                                   }
                                   s_scan = *list;
                                   s_scanned = true;
                                   render();
                               }
                               delete list;
                               delete err;
                           });
    if (!started) {
        delete list;
        delete err;
        s_scanned = true;
        render();
    }
}

// Joins the strongest saved network in range. quiet: background retry, no screen changes
// unless it connects.
static void auto_connect(bool quiet)
{
    auto* err = new esp_err_t(ESP_FAIL);
    auto* joined = new std::string();
    auto* error = new std::string();
    uint32_t gen = s_generation;
    bool started = run_job([=]() { *err = wifi::auto_connect(*joined, *error); },
                           [=]() {
                               // Shown only if the user still waits on "Joining your WiFi..."
                               bool shown_here = !quiet && gen == s_generation && s_screen == Screen::Busy;
                               if (*err == ESP_OK) {
                                   s_reconnect_delay_s = 0;
                                   s_reconnect_at = 0;
                                   if (shown_here) {
                                       connected_ok(*joined);
                                   } else {
                                       after_connect(true);
                                       refresh();
                                   }
                               } else {
                                   // Retry in the background: 15 s, doubling up to 5 min
                                   s_reconnect_delay_s = s_reconnect_delay_s ? std::min(s_reconnect_delay_s * 2, 300) : 15;
                                   s_reconnect_at = now_ms() + s_reconnect_delay_s * 1000;
                                   if (!quiet) {
                                       s_notice.add(*err == ESP_ERR_NOT_FOUND
                                                        ? "None of your saved networks is in range. Pick one:\n"
                                                        : ("Couldn't join a saved network: " + *error + "\n").c_str());
                                       if (shown_here) {
                                           scan_networks();
                                       } else {
                                           refresh();
                                       }
                                   }
                               }
                               delete err;
                               delete joined;
                               delete error;
                           });
    if (started && !quiet) {
        busy("Joining your WiFi...");
    } else if (!started) {
        delete err;
        delete joined;
        delete error;
    }
}

// ---------------------------------------------------------------------------
// Sign-in page (captive portal), as SimplrSSH's portal_menu
// ---------------------------------------------------------------------------

// http://<gateway>/ - many portals answer there when their DNS gives nothing before sign-in
static std::string gateway_url()
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.gw.addr == 0) {
        return "";
    }
    char ip[16];
    esp_ip4addr_ntoa(&info.gw, ip, sizeof(ip));
    return std::string("http://") + ip + "/";
}

static void portal_wipe()
{
    portal::wipe(s_portal);
    s_cookies.wipe();
    s_portal_field = -1;
    s_portal_selected = -1;
}

static void portal_leave(bool online)
{
    portal_wipe();
    if (online) {
        s_notice.add("You're online.\n");
        if (player_count() == 0) {
            load_stations();
        } else {
            defer([]() { player_resume(); });
        }
        go(Screen::NowPlaying);
    } else {
        go(s_link_up ? Screen::Wifi : Screen::Menu);
    }
}

struct PortalJob {
    bool has_request = false;
    portal::Request request;
    std::string gateway;
    portal::Cookies cookies;
    portal::Result result;
    esp_err_t err = ESP_FAIL;
};

// Loads the probe (request NULL: is a sign-in needed?) or a page / form.
// check: right after connecting - nothing is shown unless the network wants a sign-in.
// show_busy: a "please wait" page meanwhile (not for background reconnects).
static void portal_load(const portal::Request* request, bool check, bool show_busy)
{
    auto* job = new PortalJob();
    job->has_request = request != NULL;
    if (request) {
        job->request = *request;
    } else {
        job->gateway = gateway_url();
    }
    job->cookies = s_cookies;
    bool started = run_job(
        [job]() {
            if (!job->has_request) {
                job->err = portal::probe(job->cookies, job->result);
                if (job->err != ESP_OK && !job->gateway.empty()) {
                    portal::Request router;
                    router.url = job->gateway;
                    portal::Result page;
                    if (portal::fetch(router, job->cookies, page) == ESP_OK) {
                        portal::wipe(job->result.page);
                        job->result = page;
                        job->err = ESP_OK;
                    }
                    portal::wipe(page.page);
                }
            } else {
                job->err = portal::fetch(job->request, job->cookies, job->result);
                if (job->err == ESP_OK) {
                    // Sent: is the internet reachable now? (The answer page is shown if not)
                    portal::Result check;
                    if (portal::probe(job->cookies, check) == ESP_OK && check.online) {
                        job->result.online = true;
                    }
                    portal::wipe(check.page);
                }
            }
        },
        [job, check]() {
            std::swap(s_cookies, job->cookies);
            if (job->result.online) {
                if (check) {
                    portal_wipe();
                    if (player_count() == 0) {
                        load_stations();
                    } else {
                        defer([]() { player_resume(); });
                    }
                    if (s_screen == Screen::Busy || s_screen == Screen::Boot) {
                        go(Screen::NowPlaying);
                    } else {
                        refresh();
                    }
                } else {
                    portal_leave(true);
                }
            } else if (job->err != ESP_OK) {
                if (check) {
                    // The check itself failed (DNS, firewall): try the stations anyway
                    if (player_count() == 0) {
                        load_stations();
                    } else {
                        defer([]() { player_resume(); });
                    }
                    if (s_screen == Screen::Busy || s_screen == Screen::Boot) {
                        go(Screen::NowPlaying);
                    }
                } else {
                    s_notice.add(("Couldn't load the page: " + job->result.error + "\n").c_str());
                    if (s_portal.url.empty()) {
                        s_notice.add("If the network needs a sign-in, try again, or sign in on a phone and share "
                                     "its connection (hotspot).\n");
                        go(Screen::Wifi);
                    } else {
                        go(Screen::PortalPage);
                    }
                }
            } else {
                portal::wipe(s_portal);
                std::swap(s_portal, job->result.page);
                s_portal_selected = -1;
                s_portal_field = -1;
                if (check) {
                    s_notice.add("This network wants you to sign in first (cafe / hotel page).\n");
                }
                go(Screen::PortalPage);
            }
            portal::wipe(job->request);
            portal::wipe(job->result.page);
            job->cookies.wipe();
            delete job;
        });
    if (started && show_busy) {
        busy(check ? "Checking the internet connection..." : "Loading the sign-in page...\n(Scripts don't run; forms, "
                                                              "checkboxes and links do.)");
    } else if (!started) {
        portal::wipe(job->request);
        delete job;
    }
}

static ui::Row portal_item_row(const portal::Page& page, const portal::Item& item, const std::string& key)
{
    switch (item.kind) {
        case portal::Item::SubmitForm:
            return ui::row(key, page.forms.size() > 1 ? "Send form " + std::to_string(item.index + 1) : "Send",
                           LV_SYMBOL_OK);
        case portal::Item::Redirect: {
            portal::Url u;
            return ui::row(key, "Continue to " + (portal::parse_url(page.redirect, u) ? u.host : "the next page"),
                           LV_SYMBOL_RIGHT);
        }
        case portal::Item::LinkItem: {
            const std::string& text = page.links[item.index].label;
            return ui::row(key, text.empty() ? "Link" : text, LV_SYMBOL_RIGHT, "link");
        }
        case portal::Item::FieldItem:
            break;
    }
    const portal::Field& f = page.fields[item.index];
    std::string label = f.label + (f.required ? " *" : "");
    switch (f.type) {
        case portal::FieldType::Checkbox:
            return ui::row(key, label, NULL, "", f.checked ? ui::Mark::Checked : ui::Mark::Unchecked);
        case portal::FieldType::Radio:
            return ui::row(key, label, NULL, "", f.checked ? ui::Mark::Chosen : ui::Mark::NotChosen);
        case portal::FieldType::Select:
            return ui::row(key, label, LV_SYMBOL_LIST,
                           f.options.empty() ? "(no choices)" : shorten(f.options[f.selected].label, 30), ui::Mark::More);
        case portal::FieldType::Submit:
        case portal::FieldType::Image:
            return ui::row(key, f.label, LV_SYMBOL_OK);
        case portal::FieldType::Password:
            return ui::row(key, label, LV_SYMBOL_EYE_CLOSE,
                           f.value.empty() ? "(empty)" : std::string(std::min<size_t>(f.value.size(), 12), '*'));
        default: {
            std::string value = shorten(portal::printable(f.value, 40), 30);
            return ui::row(key, label, LV_SYMBOL_EDIT, value.empty() ? "(empty)" : value);
        }
    }
}

static void build_portal_page(ui::Page& p)
{
    p.title = "Sign in";
    if (s_screen == Screen::PortalPage) {
        portal::Url url;
        portal::parse_url(s_portal.url, url);
        if (!s_portal.title.empty()) {
            p.title = shorten(s_portal.title, 40);
        }
        p.info = "From " + url.host + (url.https ? "" : " (not encrypted)");
        if (s_portal.status >= 400 && s_portal.status != 511) {
            p.info += "\nThe page answered with error " + std::to_string(s_portal.status) + ".";
        }
        if (!s_portal.text.empty()) {
            p.info += "\n\n" + s_portal.text;
        }
        std::vector<portal::Item> items = portal::items(s_portal);
        if (items.empty()) {
            p.info += "\n\nNothing to fill in or press here: the page probably needs scripts. Sign in on a phone and "
                      "share its connection (hotspot) instead.";
        }
        for (size_t i = 0; i < items.size(); i++) {
            p.rows.push_back(portal_item_row(s_portal, items[i], std::to_string(i + 1)));
        }
        p.rows.push_back(ui::row("r", "Start again / check connection", LV_SYMBOL_REFRESH));
        p.rows.push_back(ui::back_row("Leave the sign-in"));
        if (s_portal_selected >= 0 && (size_t)s_portal_selected < items.size()) {
            p.selected = s_portal_selected;   // Stay on the box just ticked or the field just filled
        }
    } else if (s_screen == Screen::PortalField) {
        const portal::Field& f = s_portal.fields[s_portal_field];
        bool secret = f.type == portal::FieldType::Password;
        if (secret && s_portal.url.compare(0, 6, "https:") != 0) {
            p.info = "This page isn't encrypted - others on this network could see what you type here.";
        }
        p.question = f.label + (f.required ? " *" : "");
        p.secret = secret;
        p.value = secret ? "" : f.value;
        p.max_len = portal::MAX_VALUE;
        p.hint = keys_hint("keep it", secret);
    } else {
        const portal::Field& f = s_portal.fields[s_portal_field];
        p.title = shorten(f.label, 40);
        for (size_t i = 0; i < f.options.size(); i++) {
            p.rows.push_back(ui::row(std::to_string(i + 1), f.options[i].label, NULL, "",
                                     i == f.selected ? ui::Mark::Chosen : ui::Mark::NotChosen));
        }
        p.rows.push_back(ui::back_row());
        p.selected = f.selected < f.options.size() ? (int)f.selected : 0;
    }
}

static void portal_send(int form, int submitter)
{
    std::string missing = portal::missing_required(s_portal, form);
    if (!missing.empty()) {
        s_notice.add(("Fill in first: " + missing + "\n").c_str());
        render();
        return;
    }
    portal::Request request = portal::submit(s_portal, form, submitter);
    if (request.url.empty()) {
        s_notice.add("This form can't be sent from here.\n");
        render();
        return;
    }
    portal_load(&request, false, true);
    portal::wipe(request);
}

static void portal_activate(const portal::Item& item)
{
    portal::Request request;
    switch (item.kind) {
        case portal::Item::SubmitForm:
            portal_send(item.index, -1);
            return;
        case portal::Item::Redirect:
            request.url = s_portal.redirect;
            break;
        case portal::Item::LinkItem:
            request.url = s_portal.links[item.index].url;
            break;
        case portal::Item::FieldItem: {
            portal::Field& f = s_portal.fields[item.index];
            switch (f.type) {
                case portal::FieldType::Checkbox:
                case portal::FieldType::Radio:
                    portal::toggle(s_portal, item.index);
                    render();
                    return;
                case portal::FieldType::Select:
                    s_portal_field = item.index;
                    go(Screen::PortalOption);
                    return;
                case portal::FieldType::Submit:
                case portal::FieldType::Image:
                    portal_send(f.form, item.index);
                    return;
                default:
                    s_portal_field = item.index;
                    go(Screen::PortalField);
                    return;
            }
        }
    }
    request.referer = s_portal.url;
    portal_load(&request, false, true);
}

// After joining a network. One the user chose (or the boot join): is the internet reachable,
// or does the network want a sign-in first? A background reconnect (quiet) checks nothing and
// loads no page by itself, as in SimplrSSH: playback just resumes, and if the network wants a
// sign-in again, Menu > Sign in opens it.
static void after_connect(bool quiet)
{
    s_user_offline = false;
    if (!quiet) {
        portal_wipe();
        portal_load(NULL, true, true);
    } else if (player_count() == 0) {
        load_stations();
    } else {
        defer([]() { player_resume(); });
    }
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

static void boot_status(const char* text)
{
    post(Ev::Status, 0, strdup(text));
}

static void boot_start()
{
    s_boot_failed = false;
    s_boot_status = "Starting WiFi...";
    go(Screen::Boot);
    auto* err = new esp_err_t(ESP_FAIL);
    run_job([err]() { *err = wifi::start(boot_status); },
            [err]() {
                if (*err != ESP_OK) {
                    s_boot_failed = true;
                    s_boot_status = "WiFi didn't start. Try again, or restart the radio.";
                    render();
                } else {
                    s_booted = true;
                    if (wifi::saved().empty()) {
                        s_notice.add("Welcome! Pick your WiFi network to start listening.\n");
                        scan_networks();
                    } else {
                        auto_connect(false);
                    }
                }
                delete err;
            });
}

// ---------------------------------------------------------------------------
// Answers (a row's key, typed or tapped) and typed text
// ---------------------------------------------------------------------------

static void back();

static void answer(const std::string& raw)
{
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    int n = number_value(key);
    if (key == "0") {
        back();
        return;
    }

    switch (s_screen) {
        case Screen::Boot:
            if (key == "1") {
                boot_start();
            } else if (key == "2") {
                esp_restart();
            }
            break;

        case Screen::Menu:
            if (key == "1") {
                go(Screen::Stations);
            } else if (key == "2") {
                go(Screen::Wifi);
            } else if (key == "3") {
                go(Screen::Search);
            } else if (key == "4") {
                if (!s_link_up) {
                    s_notice.add("Connect to a network first.\n");
                    render();
                } else {
                    portal_wipe();
                    portal_load(NULL, false, true);
                }
            } else if (key == "5") {
                go(Screen::About);
            }
            break;

        case Screen::Stations:
            if (key == "r") {
                load_stations();
                go(Screen::NowPlaying);
            } else if (n >= 1 && n <= player_count()) {
                player_select(n - 1);
                go(Screen::NowPlaying);
            }
            break;

        case Screen::Wifi:
            if (key == "1") {
                scan_networks();
            } else if (key == "2") {
                go(Screen::WifiSaved);
            } else if (key == "3") {
                go(Screen::WifiForget);
            } else if (key == "4") {
                if (s_link_up) {
                    s_user_offline = true;   // The user chose this: no background reconnect
                    s_reconnect_at = 0;
                    defer([]() { player_stop(); });
                    wifi::disconnect();
                    s_notice.add("Disconnected. Pick a network to connect again.\n");
                }
                render();
            } else if (key == "5") {
                if (!s_link_up) {
                    s_notice.add("Connect to the network first, then open its sign-in page.\n");
                    render();
                } else {
                    portal_wipe();
                    portal_load(NULL, false, true);
                }
            }
            break;

        case Screen::WifiScan: {
            int count = (int)s_scan.size();
            if (!s_scanned) {
                break;
            } else if (n >= 1 && n <= count) {
                const wifi::Network net = s_scan[n - 1];
                choose_network(net.ssid, net.open, false);
            } else if (n == count + 1) {
                go(Screen::WifiHidden);
            } else if (n == count + 2) {
                scan_networks();
            }
            break;
        }

        case Screen::WifiSaved:
        case Screen::WifiForget: {
            std::vector<wifi::Saved> saved = wifi::saved();
            if (n >= 1 && n <= (int)saved.size()) {
                s_ssid = saved[n - 1].ssid;
                if (s_screen == Screen::WifiForget) {
                    go(Screen::WifiForgetConfirm);
                } else {
                    choose_network(saved[n - 1].ssid, !saved[n - 1].has_password, saved[n - 1].hidden);
                }
            }
            break;
        }

        case Screen::WifiForgetConfirm:
            if (key == "y") {
                wifi::forget(s_ssid);
                s_notice.add(("Forgot " + shown(s_ssid) + ".\n").c_str());
                go(Screen::Wifi);
            } else if (key == "n") {
                go(Screen::Wifi);
            }
            break;

        case Screen::Search:
            if (key == "1") {
                go(Screen::SearchCountry);
            } else if (key == "2") {
                go(Screen::SearchState);
            } else if (key == "3") {
                go(Screen::SearchCount);
            } else if (key == "4") {
                if (!s_link_up) {
                    s_notice.add("Connect to WiFi first.\n");
                    render();
                } else {
                    load_stations();
                    go(Screen::NowPlaying);
                }
            }
            break;

        case Screen::PortalPage: {
            std::vector<portal::Item> items = portal::items(s_portal);
            if (key == "r") {
                portal_wipe();
                portal_load(NULL, false, true);
            } else if (n >= 1 && (size_t)n <= items.size()) {
                s_portal_selected = n - 1;
                portal_activate(items[n - 1]);
            }
            break;
        }

        case Screen::PortalOption: {
            if (!portal_field_valid()) {
                go(Screen::PortalPage);
                break;
            }
            portal::Field& f = s_portal.fields[s_portal_field];
            if (n >= 1 && (size_t)n <= f.options.size()) {
                f.selected = n - 1;
                s_portal_field = -1;
                go(Screen::PortalPage);
            }
            break;
        }

        default:
            break;
    }
}

static bool valid_text(const std::string& s)
{
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c < 0x7F; });
}

// The prompt's text was submitted (Enter / the keyboard's OK)
static void submit()
{
    if (!s_ui.prompt_shown()) {
        return;
    }
    std::string text = s_ui.prompt_text();
    s_ui.prompt_wipe();
    switch (s_screen) {
        case Screen::WifiHidden:
            if (text.empty()) {
                back();
            } else {
                s_ssid = text;
                s_hidden = true;
                go(Screen::WifiPassword);
            }
            break;

        case Screen::WifiPassword:
            if (text.empty() && !s_hidden) {
                s_notice.add((std::string("Type the network's password (") + board_ui()->key_back + " to go back).\n").c_str());
                render();
            } else if (!text.empty() && (text.size() < 8 || text.size() > 63)) {
                s_notice.add("WiFi passwords have 8 to 63 characters.\n");
                render();
            } else {
                connect_to(s_ssid, text, s_hidden);
            }
            break;

        case Screen::SearchCountry: {
            std::transform(text.begin(), text.end(), text.begin(), ::toupper);
            if (!text.empty() && (text.size() != 2 || !isalpha((unsigned char)text[0]) || !isalpha((unsigned char)text[1]))) {
                s_notice.add("A country code is two letters, e.g. US.\n");
                render();
                break;
            }
            station_search_t search;
            settings_get_search(&search);
            snprintf(search.country, sizeof(search.country), "%s", text.c_str());
            settings_set_search(&search);
            s_notice.add("Saved. Choose 'Load stations now' to use it.\n");
            go(Screen::Search);
            break;
        }

        case Screen::SearchState: {
            if (!valid_text(text)) {
                s_notice.add("Letters, digits and spaces only.\n");
                render();
                break;
            }
            station_search_t search;
            settings_get_search(&search);
            snprintf(search.state, sizeof(search.state), "%s", text.c_str());
            settings_set_search(&search);
            s_notice.add("Saved. Choose 'Load stations now' to use it.\n");
            go(Screen::Search);
            break;
        }

        case Screen::SearchCount: {
            int count = number_value(text);
            if (count < 1 || count > SEARCH_COUNT_MAX) {
                s_notice.add(("A number from 1 to " + std::to_string(SEARCH_COUNT_MAX) + ".\n").c_str());
                render();
                break;
            }
            station_search_t search;
            settings_get_search(&search);
            search.count = count;
            settings_set_search(&search);
            s_notice.add("Saved. Choose 'Load stations now' to use it.\n");
            go(Screen::Search);
            break;
        }

        case Screen::PortalField: {
            if (!portal_field_valid()) {
                go(Screen::PortalPage);
                break;
            }
            portal::Field& f = s_portal.fields[s_portal_field];
            if (!text.empty()) {
                portal::wipe(f.value);
                f.value = f.type == portal::FieldType::Password ? text : portal::printable(text, portal::MAX_VALUE);
            }
            s_portal_field = -1;
            go(Screen::PortalPage);
            break;
        }

        default:
            break;
    }
    vault::wipe(text);
}

// One level up
static void back()
{
    switch (s_screen) {
        case Screen::Menu:
        case Screen::Stations:
            go(Screen::NowPlaying);
            break;
        case Screen::Wifi:
        case Screen::Search:
        case Screen::About:
            go(Screen::Menu);
            break;
        case Screen::WifiScan:
            go(Screen::Wifi);
            break;
        case Screen::WifiSaved:
        case Screen::WifiForget:
        case Screen::WifiForgetConfirm:
            go(Screen::Wifi);
            break;
        case Screen::WifiHidden:
        case Screen::WifiPassword:
            if (s_scanned) {
                go(Screen::WifiScan);
            } else {
                go(Screen::Wifi);
            }
            break;
        case Screen::SearchCountry:
        case Screen::SearchState:
        case Screen::SearchCount:
            go(Screen::Search);
            break;
        case Screen::PortalField:
        case Screen::PortalOption:
            s_portal_field = -1;
            go(Screen::PortalPage);
            break;
        case Screen::PortalPage:
            portal_leave(false);
            break;
        case Screen::Busy:
            // The job keeps running; its result is dropped
            go(s_booted ? Screen::Menu : Screen::Boot);
            break;
        case Screen::Boot:
        case Screen::NowPlaying:
            break;
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static void activate_row(int row)
{
    if (row >= 0 && row < (int)s_page.rows.size()) {
        answer(s_page.rows[row].key);
    }
}

static void now_playing_key(uint32_t key)
{
    if (key >= '0' && key <= '9') {
        if (s_np_typed.size() < 3) {
            s_np_typed += (char)key;
        }
        s_typed_ms = now_ms();
        refresh_now_playing();
        return;
    }
    if (key == KEY_ENTER && !s_np_typed.empty()) {
        int n = number_value(s_np_typed);
        s_np_typed.clear();
        if (n >= 1 && n <= player_count()) {
            player_select(n - 1);
        }
        refresh_now_playing();
        return;
    }
    if (!s_np_typed.empty() && (key == KEY_ESC || key == KEY_BACKSPACE)) {
        s_np_typed.clear();
        refresh_now_playing();
        return;
    }
    switch (key) {
        case KEY_LEFT: case ',': case '<': player_step(-1); break;
        case KEY_RIGHT: case '.': case '>': player_step(1); break;
        case KEY_UP: case '+': case '=': player_set_volume(player_volume() + 5); break;
        case KEY_DOWN: case '-': case '_': player_set_volume(player_volume() - 5); break;
        case ' ': case 'm': case 'M': player_set_muted(!player_muted()); break;
        case KEY_ENTER: case 's': case 'S': go(Screen::Stations); break;
        case 'w': case 'W': go(Screen::Wifi); break;
        case KEY_ESC: case KEY_TAB: go(Screen::Menu); break;
        default: break;
    }
    refresh_now_playing();
}

static void page_key(uint32_t key)
{
    if (s_ui.prompt_shown()) {
        switch (key) {
            case KEY_ENTER: submit(); break;
            case KEY_ESC: back(); break;
            case KEY_TAB: s_ui.toggle_reveal(); break;
            case KEY_UP: case KEY_PAGE_UP: s_ui.scroll(1); break;
            case KEY_DOWN: case KEY_PAGE_DOWN: s_ui.scroll(-1); break;
            default:
                s_ui.prompt_key(key);
                s_ui.show_keyboard(false);   // Typing on the hardware keyboard: the screen's one goes
                break;
        }
        return;
    }
    switch (key) {
        case KEY_UP: s_ui.select(ui::move(s_page, s_ui.selected(), 1)); s_typed.clear(); return;
        case KEY_DOWN: s_ui.select(ui::move(s_page, s_ui.selected(), -1)); s_typed.clear(); return;
        case KEY_PAGE_UP: s_ui.scroll(1); return;
        case KEY_PAGE_DOWN: s_ui.scroll(-1); return;
        case KEY_HOME: s_ui.select(0); return;
        case KEY_END: s_ui.select((int)s_page.rows.size() - 1); return;
        // Left / Right (the T-Deck's trackball, the arrow keys): back a level / open the row
        case KEY_LEFT:
        case KEY_ESC: back(); return;
        case KEY_RIGHT:
            s_typed.clear();
            activate_row(s_ui.selected());
            return;
        case KEY_BACKSPACE:
            if (!s_typed.empty()) {
                s_typed.pop_back();
            } else {
                back();
            }
            return;
        case KEY_ENTER: {
            int row = s_typed.empty() ? s_ui.selected() : ui::find_row(s_page, s_typed);
            s_typed.clear();
            activate_row(row);
            return;
        }
        default:
            break;
    }
    if (key >= '0' && key <= '9') {
        // Type a row's number: it is highlighted; Enter opens it. With fewer than ten rows
        // (or when no longer number can match) it opens at once.
        s_typed += (char)key;
        s_typed_ms = now_ms();
        int row = ui::find_row(s_page, s_typed);
        bool longer = false;
        for (const auto& r : s_page.rows) {
            longer = longer || (r.key.size() > s_typed.size() && r.key.compare(0, s_typed.size(), s_typed) == 0);
        }
        if (row >= 0 && !longer) {
            s_typed.clear();
            activate_row(row);
        } else if (row >= 0) {
            s_ui.select(row);
        } else if (!longer) {
            s_typed.clear();
        }
        return;
    }
    if (key_is_char(key)) {
        int row = ui::find_row(s_page, std::string(1, (char)key));
        if (row >= 0) {
            activate_row(row);
        }
    }
}

static void on_key(uint32_t key)
{
    if (key == KEY_GO_HOME) {
        if (s_booted && s_screen != Screen::Boot) {
            go(Screen::NowPlaying);
        }
        return;
    }
    if (s_screen == Screen::NowPlaying) {
        now_playing_key(key);
    } else if (s_screen == Screen::Busy) {
        if (key == KEY_ESC) {
            back();
        }
    } else {
        page_key(key);
    }
}

static void on_button(int button)
{
    switch (button) {
        case NP_MENU: go(Screen::Menu); break;
        case NP_PREV: player_step(-1); break;
        case NP_NEXT: player_step(1); break;
        case NP_MUTE: player_set_muted(!player_muted()); break;
        case NP_VOL_DOWN: player_set_volume(player_volume() - 5); break;
        case NP_VOL_UP: player_set_volume(player_volume() + 5); break;
        case NP_STATIONS: go(Screen::Stations); break;
        default: break;
    }
    if (s_screen == Screen::NowPlaying) {
        refresh_now_playing();
    }
}

static void on_swipe(lv_dir_t dir)
{
    if (s_screen != Screen::NowPlaying) {
        return;
    }
    if (dir == LV_DIR_LEFT) {
        player_step(1);
    } else if (dir == LV_DIR_RIGHT) {
        player_step(-1);
    } else if (dir == LV_DIR_TOP) {
        player_set_volume(player_volume() + 10);
    } else if (dir == LV_DIR_BOTTOM) {
        player_set_volume(player_volume() - 10);
    }
    refresh_now_playing();
}

static void on_link(bool up)
{
    post(Ev::Link, up ? 1 : 0);
}

static void handle(const Event& ev)
{
    switch (ev.type) {
        case Ev::Tap:
            if (s_screen != Screen::NowPlaying && ev.arg < (int32_t)s_page.rows.size()) {
                activate_row(ev.arg);
            }
            break;
        case Ev::Back:
            back();
            break;
        case Ev::Submit:
            submit();
            break;
        case Ev::Button:
            on_button(ev.arg);
            break;
        case Ev::Swipe:
            on_swipe((lv_dir_t)ev.arg);
            break;
        case Ev::Status:
            s_boot_status = (const char*)ev.ptr;
            free(ev.ptr);
            if (s_screen == Screen::Boot) {
                render();
            }
            break;
        case Ev::Link:
            if ((ev.arg != 0) != s_link_up) {
                s_link_up = ev.arg != 0;
                s_rssi_at = 0;
                if (!s_link_up) {
                    defer([]() { player_stop(); });
                    if (s_booted && !s_user_offline && !wifi::saved().empty()) {
                        s_reconnect_delay_s = 0;
                        s_reconnect_at = now_ms() + 3000;   // Give the access point a moment
                    }
                }
                if (s_screen == Screen::NowPlaying) {
                    render();
                } else {
                    update_status();
                }
            }
            break;
        case Ev::JobDone: {
            Job* job = (Job*)ev.ptr;
            s_busy = false;
            job->done();   // Results check s_generation themselves where it matters
            delete job;
            break;
        }
    }
}

// Background work, once per loop
static void housekeeping()
{
    int64_t now = now_ms();
    if (!s_typed.empty() && now - s_typed_ms > 2500) {
        s_typed.clear();
    }
    if (!s_np_typed.empty() && now - s_typed_ms > 4000) {
        s_np_typed.clear();
        if (s_screen == Screen::NowPlaying) {
            refresh_now_playing();
        }
    }
    // Reconnect saved WiFi in the background (not while the user is choosing a network)
    bool choosing = s_screen == Screen::WifiScan || s_screen == Screen::WifiPassword || s_screen == Screen::WifiHidden ||
                    s_screen == Screen::PortalPage || s_screen == Screen::PortalField || s_screen == Screen::PortalOption ||
                    s_screen == Screen::Busy;
    if (!s_link_up && s_reconnect_at && now >= s_reconnect_at && !s_busy && !choosing) {
        s_reconnect_at = 0;
        auto_connect(true);
    }
    if (s_link_up && s_reload_at && now >= s_reload_at && !s_busy) {
        s_reload_at = 0;
        load_stations();
    }
    uint32_t v = player_version();
    if (v != s_np_version) {
        s_np_version = v;
        if (s_screen == Screen::NowPlaying) {
            refresh_now_playing();
        } else {
            update_status();
        }
    }
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "SimplrRadio %s on %s", esp_app_get_description()->version, board_name());
    board_init_power();
    // The screen first, so it is lit and drawing whatever happens next
    lv_display_t* disp = board_display_start();

    settings_nvs::InitResult nvs = settings_nvs::init();
    if (vault::init()) {
        s_notice.add("Saved WiFi passwords couldn't be read and were reset.\n");
    }
    if (nvs == settings_nvs::InitResult::UNAVAILABLE) {
        s_notice.add("Settings can't be saved on this device.\n");
    } else if (nvs == settings_nvs::InitResult::REFORMATTED) {
        s_notice.add("Saved settings were unreadable and have been reset.\n");
    }

    s_events = xQueueCreate(32, sizeof(Event));
    s_jobs = xQueueCreate(4, sizeof(Job*));
    xTaskCreate(worker_task, "worker", 12288, NULL, 5, NULL);

    if (!disp) {
        // Nothing can be set up or shown without the screen; the log says why
        ESP_LOGE(TAG, "Display failed to start; stopping");
        vTaskSuspend(NULL);
    }
    if (lvgl_port_lock(0)) {
        s_ui.create(lv_screen_active());
        s_ui.on_tap = [](int row) { post(Ev::Tap, row); };
        s_ui.on_back = []() { post(Ev::Back); };
        s_ui.on_submit = []() { post(Ev::Submit); };
        s_ui.on_button = [](int button) { post(Ev::Button, button); };
        s_ui.on_swipe = [](lv_dir_t dir) { post(Ev::Swipe, (int32_t)dir); };
        render();
        lvgl_port_unlock();
    }

    board_keyboard_init();
    if (player_init() != ESP_OK) {
        s_notice.add("The audio output failed to start.\n");
    }
    wifi::on_link(on_link);

    if (lvgl_port_lock(0)) {
        boot_start();
        lvgl_port_unlock();
    }

    for (;;) {
        Event ev;
        bool got = xQueueReceive(s_events, &ev, pdMS_TO_TICKS(20)) == pdTRUE;
        uint32_t key = board_read_key();
        if (got || key) {
            lvgl_port_lock(0);
            if (got) {
                handle(ev);
                while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
                    handle(ev);
                }
            }
            for (int i = 0; key && i < 16; i++) {
                on_key(key);
                key = board_read_key();
            }
            lvgl_port_unlock();
        }
        // Player work that waits on its tasks, never under the display lock
        std::vector<std::function<void()>> deferred;
        deferred.swap(s_deferred);
        for (auto& fn : deferred) {
            fn();
        }
        player_tick();
        lvgl_port_lock(0);
        housekeeping();
        lvgl_port_unlock();
    }
}
