/*
 * SimplrRadio app: boot, screens and input.
 *
 * Tasks
 *   app task (app_main)  owns every screen and the player. Waits on one event queue (keys,
 *                        touches handed over from LVGL, finished jobs, WiFi link changes),
 *                        polls the keyboard, ticks the player. Holds the display lock
 *                        while it changes the screen, never while it waits.
 *   worker               runs one blocking job at a time (WiFi start / scan / connect, the
 *                        station directory, sign-in pages, Bluetooth start / stop / scan /
 *                        pair) and posts its result back, so the
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
#include "bt_hid.hpp"
#include "bt_store.hpp"
#include "crash_note.h"
#include "keys.h"
#include "music_online.h"
#include "music_player.h"
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
#include "esp_random.h"
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
    Bluetooth,
    BtScan,
    BtForgetConfirm,
    PortalPage,
    PortalField,     // Prompt
    PortalOption,
    Music,           // Songs on the SD card
    MusicSongs,      // A list of songs (all, an artist's, an album, a genre, favorites, a search)
    MusicGroups,     // Artists, albums or genres
    MusicSearch,     // Prompt
    MusicSettings,
    MusicFolder,     // Choosing the music folder
};

static UiView s_ui;
static Screen s_screen = Screen::Boot;
static ui::Page s_page;                 // What the screen shows (menus, prompts)
static ui::Notice s_notice;             // Results and errors, shown on the next page
static std::string s_last_restart;      // Why the radio last restarted, if not a normal start (About)
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

// Bluetooth keyboards and remotes
static std::vector<bt_hid::Found> s_bt_found;
static bool s_bt_scanned;
static bool s_bt_switching;             // Bluetooth is being turned on or off
static bool s_bt_autostart;             // Turn it on once the radio is up (it was on)
static bt_store::Device s_bt_forget;
static uint32_t s_bt_version = UINT32_MAX;

// Music
static const int MUSIC_PAGE_ROWS = 40;  // Rows of a long list per page
static bool s_music_opening;            // The card / the library is being read
static std::string s_music_problem;     // Why there are no songs ("No SD card found...")
static int s_music_scanned_shown = -1;  // Songs found, as the "Please wait" screen shows it
static music::Indexes s_songs;          // The list MusicSongs shows
static std::string s_songs_title;
static Screen s_songs_back = Screen::Music;
static int s_songs_page;
static music::Groups s_groups;          // The list MusicGroups shows
static music::Group s_group_kind = music::Group::Artist;
static int s_groups_page;
static std::string s_folder = "/";      // The folder browser's folder
static std::vector<std::string> s_folders;
static int s_folders_page;
static int64_t s_lookup_at;             // Next online genre lookup (0 = none due)
static int s_lookup_pos;                // Where in the library the lookups have got to
static int s_lookup_dirty;              // Genres found since the index was saved

// Last status shown in the title bar
static uint32_t s_np_version = UINT32_MAX;

static int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

static void defer(std::function<void()> fn);
static void go(Screen screen);
static void render();
static void refresh();
static void busy(const std::string& text);
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
    s_ui.set_status(s_link_up, s_link_up ? s_rssi : -100, player_volume(), player_muted(),
                    bt_hid::state() == bt_hid::State::Connected);
}

static void music_now_playing(NowPlaying& np);

static void refresh_now_playing()
{
    NowPlaying np;
    s_ui.set_title(music_active() ? "SimplrRadio - Music" : s_link_up ? "SimplrRadio" : "SimplrRadio - offline");
    s_ui.set_list_button(music_active() ? "Songs" : "Stations");
    if (music_active()) {
        music_now_playing(np);
        s_ui.update_now_playing(np);
        update_status();
        return;
    }
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
    np.paused = player_paused();
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
                if (player_error()[0]) {
                    np.status = std::string("Station unavailable (") + player_error() + ") - try the next one.";
                }
                np.status_warn = true;
                break;
            case PLAYER_IDLE: np.status = "Stopped"; break;
            case PLAYER_PAUSED: np.status = "Paused - press Play to listen again"; break;
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

static bool bt_on()
{
    bt_hid::State st = bt_hid::state();
    return st != bt_hid::State::Off && st != bt_hid::State::Unavailable;
}

static std::string bt_name(const std::string& name)
{
    return name.empty() ? std::string("Unnamed device") : name;
}

static std::string bt_address(const uint8_t addr[6])
{
    char text[8];
    snprintf(text, sizeof(text), "%02X:%02X", addr[1], addr[0]);   // The last two bytes, to tell twins apart
    return text;
}

// The Bluetooth page's status line
static std::string bt_status()
{
    uint32_t code = 0;
    if (s_bt_switching) {
        return bt_on() ? "Turning Bluetooth off..." : "Starting Bluetooth...";
    }
    if (bt_hid::passkey(code)) {
        char text[96];
        snprintf(text, sizeof(text), "Type %06u on the keyboard, then press Enter.", (unsigned)code);
        return text;
    }
    std::string device = bt_name(bt_hid::device());
    switch (bt_hid::state()) {
        case bt_hid::State::Off:
            return "Off. Turn it on to use a Bluetooth keyboard or remote: media keys, arrows and typing "
                   "work the radio. (Bluetooth LE only: headphones and speakers can't connect.)";
        case bt_hid::State::Unavailable:
            if (board_wifi_firmware()) {
                return std::string("Bluetooth didn't start: the WiFi chip's firmware (") + board_wifi_firmware() +
                       ") has no Bluetooth controller available.";
            }
            return "Bluetooth didn't start on this radio (its radio chip has no Bluetooth controller "
                   "available).";
        case bt_hid::State::Scanning: return "Looking for keyboards and remotes...";
        case bt_hid::State::Connecting: return "Connecting to " + device + "...";
        case bt_hid::State::Pairing: return "Pairing with " + device + "...";
        case bt_hid::State::Setup: return "Setting up " + device + "...";
        case bt_hid::State::Connected: return "Connected: " + device + ".";
        case bt_hid::State::Idle: break;
    }
    return bt_hid::waiting() ? "On. Your paired devices connect by themselves when they are switched on."
                             : "On. No device paired yet: pick 'Pair a keyboard or remote'.";
}

static std::string bt_summary()
{
    if (bt_hid::state() == bt_hid::State::Connected) {
        return bt_name(bt_hid::device());
    }
    return bt_hid::state() == bt_hid::State::Unavailable ? "not available" : bt_on() ? "on" : "off";
}

// ---------------------------------------------------------------------------
// Music (songs on the SD card): the same pages as the radio's menus
// ---------------------------------------------------------------------------

static std::string plural(int n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

static int pages_of(int count)
{
    return count <= MUSIC_PAGE_ROWS ? 1 : (count + MUSIC_PAGE_ROWS - 1) / MUSIC_PAGE_ROWS;
}

// "Previous page" / "Next page" rows and "Page 2 of 5" for long lists
static void add_pager(ui::Page& p, int page, int count)
{
    int pages = pages_of(count);
    if (pages <= 1) {
        return;
    }
    if (page > 0) {
        p.rows.push_back(ui::row("p", "Previous page", LV_SYMBOL_UP));
    }
    if (page + 1 < pages) {
        p.rows.push_back(ui::row("n", "Next page", LV_SYMBOL_DOWN, std::to_string(page + 2) + " of " + std::to_string(pages)));
    }
}

static std::string page_info(int page, int count)
{
    return pages_of(count) > 1 ? "Page " + std::to_string(page + 1) + " of " + std::to_string(pages_of(count)) + "   " : "";
}

static bool page_key_move(const std::string& key, int& page, int count)
{
    if (key == "n" && page + 1 < pages_of(count)) {
        page++;
    } else if (key == "p" && page > 0) {
        page--;
    } else {
        return false;
    }
    return true;
}

static const char* repeat_words(music::Repeat r)
{
    return r == music::Repeat::Off ? "off" : r == music::Repeat::One ? "this song" : "the list";
}

static std::string song_detail(const music::Track& t)
{
    std::string d = t.artist[0] ? t.artist : "Unknown artist";
    std::string time = music::duration_text(t.duration_ms);
    if (!time.empty()) {
        d += "   " + time;
    }
    if (t.flags & music::UNPLAYABLE) {
        d += "   can't be played";
    }
    return d;
}

static std::string music_summary()
{
    if (s_music_opening) {
        return "reading the SD card...";
    }
    if (!music::loaded()) {
        return s_music_problem.empty() ? "SD card" : "no songs";
    }
    return plural(music::library().size(), "song", "songs");
}

// Opens the song list `songs` (titled `title`; Back returns to `from`); the playing song is
// highlighted when it is on the list
static void show_songs(const std::string& title, const music::Indexes& songs, Screen from)
{
    s_songs = songs;
    s_songs_title = title;
    s_songs_back = from;
    s_songs_page = 0;
    int playing = music::playing();
    for (size_t i = 0; i < s_songs.size(); i++) {
        if (s_songs[i] == playing) {
            s_songs_page = (int)i / MUSIC_PAGE_ROWS;
        }
    }
    go(Screen::MusicSongs);
}

static void show_groups(music::Group kind)
{
    s_group_kind = kind;
    s_groups = music::library().groups(kind);
    s_groups_page = 0;
    go(Screen::MusicGroups);
}

// Plays songs[start] and the rest of the list after it; past the display lock (it stops a song)
static void play_songs(const music::Indexes& songs, int start)
{
    if (songs.empty()) {
        return;
    }
    music::Indexes copy = songs;
    defer([copy, start]() { music::play(copy, start); });
    go(Screen::NowPlaying);
}

// Why the card couldn't be mounted, for the screen. ESP_FAIL: the card answered but holds no
// FAT file system (FAT32 or FAT16; exFAT, which cards over 32 GB come with, isn't built in);
// anything else: no card answered.
static std::string card_problem(esp_err_t err)
{
    if (err == ESP_FAIL) {
        return "The SD card isn't FAT32. Cards over 32 GB come as exFAT, which the radio can't read: "
               "format it as FAT32 on a computer.";
    }
    return std::string("No SD card found (") + esp_err_to_name(err) +
           "). Put in a microSD card (FAT32) with your music.";
}

// Reads the library: the card is mounted, the index of the chosen folder read (or the folder
// scanned when there is none, it doesn't match, or `rescan`). quiet: no "Please wait" screen
// (at boot). `then` runs on the app task when it is done, whatever happened.
static void music_open(bool rescan, bool quiet, std::function<void()> then)
{
    struct Open {
        music::Library lib;
        music::GenreCache genres;
        music::ScanResult scan;
        esp_err_t mounted = ESP_FAIL;
        bool from_index = false;
        bool memory = true;
        std::string folder;
    };
    if (s_music_opening) {
        return;
    }
    uint32_t gen = s_generation;
    auto* job = new Open();
    job->folder = music::settings().folder.empty() ? "/" : music::settings().folder;
    bool started = run_job(
        [job, rescan]() {
            job->mounted = music::mount();
            if (job->mounted != ESP_OK) {
                return;
            }
            music::load_genres(job->genres);
            job->from_index = !rescan && music::load_index(job->folder, job->lib);
            if (!job->from_index) {
                job->memory = job->lib.reserve();
                if (job->memory) {
                    job->scan = music::scan(job->folder, job->lib, job->genres);
                    if (job->scan.card) {
                        music::save_index(job->folder, job->lib);
                    }
                }
            }
        },
        [job, then, quiet, gen]() {
            s_music_opening = false;
            s_music_problem.clear();
            if (job->mounted != ESP_OK) {
                s_music_problem = card_problem(job->mounted) + " Then choose Settings > Scan the folder again.";
            } else if (!job->memory) {
                s_music_problem = "Not enough memory for the music library.";
            } else if (!job->from_index && !job->scan.card) {
                s_music_problem = "The music folder " + portal::printable(job->folder, 60) +
                                  " isn't on this card. Choose another in Settings > Music folder.";
            } else {
                music::genres() = job->genres;
                music::install(job->lib);
                s_songs.clear();   // Lists of the old library's songs mean nothing now
                s_groups.clear();
                s_lookup_pos = 0;
                int songs = music::library().size();
                if (songs == 0) {
                    s_music_problem = "No songs in " + music::folder_shown() +
                                      ". The radio plays MP3, AAC, M4A, ALAC, FLAC, Ogg Vorbis, Opus, WAV and AMR files.";
                } else if (!job->from_index) {
                    std::string found = "Found " + plural(songs, "song", "songs") + " in " + music::folder_shown() + ".";
                    if (job->scan.full) {
                        found += " The library is full (" + std::to_string(music::MAX_TRACKS) + " songs): choose a smaller folder.";
                    }
                    if (job->scan.unreadable) {
                        found += " " + plural(job->scan.unreadable, "file", "files") + " couldn't be read.";
                    }
                    if (job->scan.too_long) {
                        found += " " + plural(job->scan.too_long, "file or folder", "files or folders") +
                                 " left out (path too long, or more than " + std::to_string(music::MAX_FOLDER_DEPTH) +
                                 " folders deep).";
                    }
                    s_notice.add((found + "\n").c_str());
                }
                s_lookup_at = now_ms() + 5000;   // Missing genres: looked up online soon
            }
            if (!s_music_problem.empty()) {
                ESP_LOGW(TAG, "Music library not read");
            }
            delete job;
            if (then && (quiet || gen == s_generation)) {
                then();   // Unless the user left the "Please wait" screen meanwhile
            } else {
                refresh();
            }
        });
    if (!started) {
        delete job;
        if (then) {
            then();
        }
        return;
    }
    s_music_opening = true;
    if (!quiet) {
        s_music_scanned_shown = -1;
        busy(rescan ? "Reading your music folder..." : "Reading the SD card...");
    }
}

// The folder browser: lists `folder`'s folders (worker: the card is read)
static void browse(const std::string& folder)
{
    auto* list = new std::vector<std::string>();
    auto* ok = new bool(false);
    auto* mounted = new esp_err_t(ESP_OK);
    bool started = run_job(
        [folder, list, ok, mounted]() {
            *mounted = music::mount();
            if (*mounted == ESP_OK) {
                *list = music::list_folders(folder, *ok);
            }
        },
        [folder, list, ok, mounted]() {
            if (*ok) {
                s_folder = folder;
                s_folders = *list;
                s_folders_page = 0;
                go(Screen::MusicFolder);
            } else {
                s_notice.add(((*mounted != ESP_OK ? card_problem(*mounted) : "The SD card couldn't be read.") + "\n").c_str());
                go(Screen::MusicSettings);
            }
            delete list;
            delete ok;
            delete mounted;
        });
    if (started) {
        busy("Reading the SD card...");
    } else {
        delete list;
        delete ok;
        delete mounted;
    }
}

static std::string parent_folder(const std::string& folder)
{
    size_t slash = folder.find_last_of('/');
    return slash == std::string::npos || slash == 0 ? "/" : folder.substr(0, slash);
}

// Saves the index and the genre cache after online lookups changed genres (worker)
static void save_music()
{
    if (!music::loaded() || s_busy) {
        return;
    }
    std::string folder = music::settings().folder.empty() ? "/" : music::settings().folder;
    if (run_job([folder]() {
                    music::save_index(folder, music::library());
                    music::save_genres(music::genres());
                },
                []() {})) {
        s_lookup_dirty = 0;
    }
}

// One artist's genre from MusicBrainz, when songs have none (housekeeping, one at a time)
static void lookup_next_genre()
{
    if (!music::loaded() || !music::settings().online || !s_link_up || s_busy || s_music_opening || !s_lookup_at ||
        now_ms() < s_lookup_at) {
        return;
    }
    music::GenreCache& cache = music::genres();
    music::Library& lib = music::library();
    s_lookup_pos = lib.next_without_genre(s_lookup_pos, [&cache](const char* artist) { return cache.known(artist); });
    if (s_lookup_pos < 0) {
        s_lookup_pos = 0;
        s_lookup_at = 0;   // Every artist was asked about (until the next scan)
        if (s_lookup_dirty) {
            save_music();
        }
        return;
    }
    struct Lookup {
        music_online_result_t result = MUSIC_ONLINE_ERROR;
        char genre[MEDIA_GENRE_LEN] = "";
        char name[MEDIA_NAME_LEN] = "";
    };
    std::string artist = lib.at(s_lookup_pos).artist;
    auto* res = new Lookup();
    bool started = run_job(
        [artist, res]() {
            res->result = music_online_artist(artist.c_str(), res->genre, sizeof(res->genre), res->name, sizeof(res->name));
        },
        [artist, res]() {
            int64_t now = now_ms();
            switch (res->result) {
                case MUSIC_ONLINE_FOUND:
                    music::library().set_artist_genre(artist, res->genre);
                    if (res->name[0]) {
                        music::library().set_artist_name(artist, res->name);
                    }
                    music::genres().put(res->name[0] ? res->name : artist, res->genre);
                    s_lookup_dirty++;
                    s_lookup_at = now + 1500;
                    break;
                case MUSIC_ONLINE_NONE:
                    music::genres().put(artist, "");   // Not asked again
                    s_lookup_dirty++;
                    s_lookup_at = now + 1500;
                    break;
                case MUSIC_ONLINE_BUSY: s_lookup_at = now + 60000; break;
                case MUSIC_ONLINE_ERROR: s_lookup_at = now + 30000; break;
            }
            delete res;
            if (s_lookup_dirty >= 10) {
                save_music();
            }
            if (s_screen == Screen::MusicGroups) {
                s_groups = music::library().groups(s_group_kind);   // Genres move between groups
                s_groups_page = std::min(s_groups_page, pages_of((int)s_groups.size()) - 1);
            }
            if (s_screen == Screen::Music || s_screen == Screen::MusicGroups || s_screen == Screen::MusicSongs) {
                refresh();
            }
        });
    if (!started) {
        delete res;
    }
}

// Songs without a genre (a count over the library, cheap enough for every redraw)
static int songs_without_genre()
{
    int n = 0;
    music::Library& lib = music::library();
    for (int i = 0; i < lib.size(); i++) {
        n += !lib.at(i).genre[0];
    }
    return n;
}

static void build_music_page(ui::Page& p)
{
    music::Library& lib = music::library();
    switch (s_screen) {
        case Screen::Music: {
            p.title = "Music";
            if (s_music_opening) {
                p.info = "Reading the SD card...";
                p.busy = true;
            } else if (!s_music_problem.empty()) {
                p.info = s_music_problem;
            } else if (music::loaded()) {
                p.info = plural(lib.size(), "song", "songs") + " in " + music::folder_shown() + " on the SD card.";
                int left = music::settings().online && s_lookup_at ? songs_without_genre() : 0;
                if (left) {
                    p.info += "\nLooking up genres online for " + plural(left, "song", "songs") + " without one...";
                }
            }
            int artists = music::loaded() ? (int)lib.groups(music::Group::Artist).size() : 0;
            int albums = music::loaded() ? (int)lib.groups(music::Group::Album).size() : 0;
            int genres = music::loaded() ? (int)lib.groups(music::Group::Genre).size() : 0;
            p.rows = {
                ui::row("1", "Songs", LV_SYMBOL_AUDIO, music::loaded() ? std::to_string(lib.size()) : "", ui::Mark::More),
                ui::row("2", "Artists", LV_SYMBOL_LIST, artists ? std::to_string(artists) : "", ui::Mark::More),
                ui::row("3", "Albums", LV_SYMBOL_DIRECTORY, albums ? std::to_string(albums) : "", ui::Mark::More),
                ui::row("4", "Genres", LV_SYMBOL_SHUFFLE, genres ? std::to_string(genres) : "", ui::Mark::More),
                ui::row("5", "Favorites", LV_SYMBOL_OK, std::to_string(lib.with_hashes(music::favorites()).size()),
                        ui::Mark::More),
                ui::row("6", "Search", LV_SYMBOL_EDIT, "title, artist or album", ui::Mark::More),
                ui::row("7", "Shuffle all songs", LV_SYMBOL_SHUFFLE),
            };
            int playing = music::playing();
            if (playing >= 0) {
                bool fav = music::is_favorite(playing);
                p.rows.push_back(ui::row("f", fav ? "Remove the song playing from favorites" : "Add the song playing to favorites",
                                         LV_SYMBOL_OK, shorten(lib.at(playing).title, 40)));
            }
            p.rows.push_back(ui::row("8", "Settings", LV_SYMBOL_SETTINGS, "folder, shuffle, repeat", ui::Mark::More));
            p.rows.push_back(ui::back_row());
            break;
        }

        case Screen::MusicSongs: {
            p.title = s_songs_title;
            int count = (int)s_songs.size();
            p.info = count ? page_info(s_songs_page, count) + plural(count, "song", "songs") : "No songs here.";
            if (count) {
                p.rows.push_back(ui::row("a", "Play all", LV_SYMBOL_PLAY));
                p.rows.push_back(ui::row("s", "Shuffle all", LV_SYMBOL_SHUFFLE));
            }
            int first = s_songs_page * MUSIC_PAGE_ROWS;
            int playing = music::playing();
            for (int i = first; i < count && i < first + MUSIC_PAGE_ROWS; i++) {
                if (s_songs[i] >= lib.size()) {
                    continue;
                }
                const music::Track& t = lib.at(s_songs[i]);
                bool here = s_songs[i] == playing;
                p.rows.push_back(ui::row(std::to_string(i - first + 1), t.title, music::is_favorite(s_songs[i]) ? LV_SYMBOL_OK : LV_SYMBOL_AUDIO,
                                         song_detail(t), here ? ui::Mark::Chosen : ui::Mark::None));
                if (here) {
                    p.selected = (int)p.rows.size() - 1;
                }
            }
            add_pager(p, s_songs_page, count);
            p.rows.push_back(ui::back_row());
            break;
        }

        case Screen::MusicGroups: {
            const char* what = s_group_kind == music::Group::Artist ? "Artists"
                               : s_group_kind == music::Group::Album ? "Albums" : "Genres";
            p.title = what;
            int count = (int)s_groups.size();
            std::string words = s_group_kind == music::Group::Artist ? "artists"
                                : s_group_kind == music::Group::Album ? "albums" : "genres";
            p.info = count ? page_info(s_groups_page, count) + std::to_string(count) + " " + words : "No songs yet.";
            int first = s_groups_page * MUSIC_PAGE_ROWS;
            for (int i = first; i < count && i < first + MUSIC_PAGE_ROWS; i++) {
                const music::GroupEntry& g = s_groups[i];
                if (g.first >= lib.size()) {
                    continue;
                }
                const music::Track& t = lib.at(g.first);
                std::string detail = plural(g.count, "song", "songs");
                if (s_group_kind == music::Group::Album && t.artist[0]) {
                    detail = std::string(t.artist) + "   " + detail;
                }
                p.rows.push_back(ui::row(std::to_string(i - first + 1), music::group_name(t, s_group_kind), NULL, detail,
                                         ui::Mark::More));
            }
            add_pager(p, s_groups_page, count);
            p.rows.push_back(ui::back_row());
            break;
        }

        case Screen::MusicSearch:
            p.title = "Search";
            p.question = "Words in the title, artist or album";
            p.hint = keys_hint("search", false);
            p.max_len = 40;
            break;

        case Screen::MusicSettings: {
            music::Settings& s = music::settings();
            p.title = "Music settings";
            p.info = "Genres missing from your files are looked up on musicbrainz.org (only the artist's name is sent).";
            p.rows = {
                ui::row("1", "Music folder", LV_SYMBOL_DIRECTORY, music::folder_shown(), ui::Mark::More),
                ui::row("2", "Scan the folder again", LV_SYMBOL_REFRESH,
                        music::loaded() ? plural(music::library().size(), "song", "songs") + " now" : ""),
                ui::row("3", "Shuffle", LV_SYMBOL_SHUFFLE, s.shuffle ? "on" : "off", s.shuffle ? ui::Mark::On : ui::Mark::Off),
                ui::row("4", "Repeat", LV_SYMBOL_LOOP, repeat_words(s.repeat)),
                ui::row("5", "Look up missing genres online", LV_SYMBOL_WIFI, s.online ? "on" : "off",
                        s.online ? ui::Mark::On : ui::Mark::Off),
                ui::back_row(),
            };
            break;
        }

        case Screen::MusicFolder: {
            p.title = "Music folder";
            int count = (int)s_folders.size();
            p.info = "On the SD card: " + (s_folder == "/" ? std::string("the top folder") : portal::printable(s_folder, 80)) +
                     "\n" + page_info(s_folders_page, count) +
                     (count ? "Open a folder, or use this one." : "No folders in it. Use this one?");
            p.rows.push_back(ui::row("u", "Use this folder", LV_SYMBOL_OK));
            if (s_folder != "/") {
                p.rows.push_back(ui::row("b", "Up one level", LV_SYMBOL_UP));
            }
            int first = s_folders_page * MUSIC_PAGE_ROWS;
            for (int i = first; i < count && i < first + MUSIC_PAGE_ROWS; i++) {
                p.rows.push_back(ui::row(std::to_string(i - first + 1), portal::printable(s_folders[i], 60),
                                         LV_SYMBOL_DIRECTORY, "", ui::Mark::More));
            }
            add_pager(p, s_folders_page, count);
            p.rows.push_back(ui::back_row());
            break;
        }

        default:
            break;
    }
}

static void back();

static void music_answer(const std::string& key, int n)
{
    music::Library& lib = music::library();
    switch (s_screen) {
        case Screen::Music:
            if (key == "8") {
                go(Screen::MusicSettings);
                break;
            }
            if (!music::loaded() || lib.size() == 0) {
                if (key >= "1" && key <= "7" && key.size() == 1) {
                    s_notice.add(s_music_problem.empty() ? "No songs yet.\n" : (s_music_problem + "\n").c_str());
                    render();
                }
                break;
            }
            if (key == "1") {
                show_songs("Songs", lib.all(), Screen::Music);
            } else if (key == "2") {
                show_groups(music::Group::Artist);
            } else if (key == "3") {
                show_groups(music::Group::Album);
            } else if (key == "4") {
                show_groups(music::Group::Genre);
            } else if (key == "5") {
                music::Indexes favs = lib.with_hashes(music::favorites());
                if (favs.empty()) {
                    s_notice.add("No favorites yet. While a song plays, choose 'Add the song playing to favorites' "
                                 "here (or press F on Now Playing).\n");
                    render();
                } else {
                    show_songs("Favorites", favs, Screen::Music);
                }
            } else if (key == "6") {
                go(Screen::MusicSearch);
            } else if (key == "7") {
                music::Indexes all = lib.all();
                music::set_shuffle(true);
                play_songs(all, (int)(esp_random() % all.size()));
            } else if (key == "f" && music::playing() >= 0) {
                int song = music::playing();
                bool was = music::is_favorite(song);
                if (!music::toggle_favorite(song)) {
                    s_notice.add(("The favorites list is full (" + std::to_string(music::MAX_FAVORITES) + " songs).\n").c_str());
                } else {
                    s_notice.add(was ? "Removed from favorites.\n" : "Added to favorites.\n");
                }
                render();
            }
            break;

        case Screen::MusicSongs: {
            int count = (int)s_songs.size();
            if (page_key_move(key, s_songs_page, count)) {
                render();
            } else if ((key == "a" || key == "s") && count) {
                music::set_shuffle(key == "s");
                play_songs(s_songs, key == "s" ? (int)(esp_random() % count) : 0);
            } else if (n >= 1 && n <= MUSIC_PAGE_ROWS) {
                int i = s_songs_page * MUSIC_PAGE_ROWS + n - 1;
                if (i < count) {
                    play_songs(s_songs, i);
                }
            }
            break;
        }

        case Screen::MusicGroups: {
            int count = (int)s_groups.size();
            if (page_key_move(key, s_groups_page, count)) {
                render();
            } else if (n >= 1 && n <= MUSIC_PAGE_ROWS) {
                int i = s_groups_page * MUSIC_PAGE_ROWS + n - 1;
                if (i < count && s_groups[i].first < lib.size()) {
                    const music::Track& t = lib.at(s_groups[i].first);
                    show_songs(music::group_name(t, s_group_kind), lib.in_group(s_group_kind, s_groups[i].first),
                               Screen::MusicGroups);
                }
            }
            break;
        }

        case Screen::MusicSettings: {
            music::Settings& s = music::settings();
            if (key == "1") {
                browse(s.folder.empty() ? "/" : s.folder);
            } else if (key == "2") {
                music_open(true, false, []() { go(Screen::Music); });
            } else if (key == "3") {
                music::set_shuffle(!s.shuffle);
                render();
            } else if (key == "4") {
                music::set_repeat(s.repeat == music::Repeat::Off ? music::Repeat::All
                                  : s.repeat == music::Repeat::All ? music::Repeat::One : music::Repeat::Off);
                render();
            } else if (key == "5") {
                s.online = !s.online;
                music::save_settings(s);
                s_lookup_at = s.online ? now_ms() + 1000 : 0;
                render();
            }
            break;
        }

        case Screen::MusicFolder: {
            int count = (int)s_folders.size();
            if (page_key_move(key, s_folders_page, count)) {
                render();
            } else if (key == "u") {
                music::Settings& s = music::settings();
                bool same = s.folder == s_folder || (s.folder.empty() && s_folder == "/");
                s.folder = s_folder;
                music::save_settings(s);
                music_open(!same, false, []() { go(Screen::Music); });
            } else if (key == "b" && s_folder != "/") {
                browse(parent_folder(s_folder));
            } else if (n >= 1 && n <= MUSIC_PAGE_ROWS) {
                int i = s_folders_page * MUSIC_PAGE_ROWS + n - 1;
                if (i < count) {
                    browse((s_folder == "/" ? "" : s_folder) + "/" + s_folders[i]);
                }
            }
            break;
        }

        default:
            break;
    }
}

// Now Playing while a song plays
static void music_now_playing(NowPlaying& np)
{
    const music::Queue& q = music::queue();
    int song = music::playing();
    if (song >= 0 && song < music::library().size()) {
        const music::Track& t = music::library().at(song);
        np.station = t.title;
        np.song = t.artist[0] ? t.artist : "Unknown artist";
        if (t.album[0]) {
            np.song += std::string("  -  ") + t.album;
        }
        np.info = std::to_string(q.list_position() + 1) + " / " + std::to_string(q.size()) + "   " +
                  media_format_name((media_format_t)t.format);
        if (t.genre[0]) {
            np.info += std::string("   ") + t.genre;
        }
        std::string total = music::duration_text(t.duration_ms);
        std::string elapsed = music::duration_text(music::elapsed_ms());
        np.info += "   " + (elapsed.empty() ? std::string("0:00") : elapsed) + (total.empty() ? "" : " / " + total);
        if (music::is_favorite(song)) {
            np.info += "   favorite";
        }
        if (q.shuffled()) {
            np.info += "   shuffle";
        }
    }
    np.muted = player_muted();
    np.paused = player_paused();
    if (!s_np_typed.empty()) {
        np.status = "Go to song " + s_np_typed + "_   (Enter)";
        return;
    }
    switch (player_status()) {
        case PLAYER_CONNECTING: np.status = "Loading..."; break;
        case PLAYER_UNAVAILABLE:
            np.status = "This file can't be played - skipping it.";
            np.status_warn = true;
            break;
        case PLAYER_IDLE: np.status = "End of the list - press Play to hear it again."; break;
        case PLAYER_PAUSED: np.status = "Paused - press Play to listen again"; break;
        case PLAYER_PLAYING: np.status = np.muted ? "Muted" : ""; break;
    }
}

// The list behind the List button / Enter on Now Playing: the songs playing, or the stations
static void open_list()
{
    if (music_active() && !music::queue().empty()) {
        show_songs("Playing", music::queue().songs(), Screen::NowPlaying);
    } else {
        go(Screen::Stations);
    }
}

// Music played last: the library is read at boot and the last song plays again
static void music_resume(std::function<void()> then)
{
    music_open(false, true, [then]() {
        music::Library& lib = music::library();
        int last = lib.find_hash(music::settings().last_song);
        if (last >= 0) {
            music::Indexes all = lib.all();
            int start = (int)(std::find(all.begin(), all.end(), (uint16_t)last) - all.begin());
            defer([all, start]() { music::play(all, start); });
            s_screen = Screen::NowPlaying;
        }
        player_set_hold(false);   // Without music the radio plays once its stations are in
        if (last < 0) {
            defer([]() { player_resume(); });
        }
        then();
    });
}

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
                ui::row("2", "Music", LV_SYMBOL_AUDIO, music_summary(), ui::Mark::More),
                ui::row("3", "WiFi", LV_SYMBOL_WIFI, up ? shown(wifi::ssid()) : "not connected", ui::Mark::More),
                ui::row("4", "Station search", LV_SYMBOL_DIRECTORY, search_summary(), ui::Mark::More),
                ui::row("5", "Sign in to this network", LV_SYMBOL_RIGHT, up ? "cafe / hotel page" : "not connected",
                        ui::Mark::More),
                ui::row("6", "Bluetooth", LV_SYMBOL_BLUETOOTH, bt_summary(), ui::Mark::More),
                ui::row("7", "About", LV_SYMBOL_HOME, "", ui::Mark::More),
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
            p.info += "Bluetooth: " + bt_summary() + "\n";
            if (!s_last_restart.empty()) {
                p.info += s_last_restart + "\n";
            }
            p.info += "Music: " + music_summary() + (music::loaded() ? " in " + music::folder_shown() : "") +
                      " (genres from musicbrainz.org)\n";
            p.info += std::string("Settings: ") + settings_nvs::partition() +
                      " partition, WiFi passwords and Bluetooth keys encrypted\n";
            p.info += "Stations from radio-browser.info";
            p.rows = {ui::back_row()};
            break;
        }

        case Screen::Bluetooth: {
            p.title = "Bluetooth";
            p.info = bt_status();
            p.rows = {
                ui::row("1", "Bluetooth", LV_SYMBOL_BLUETOOTH, bt_on() ? "on - choose to turn off" : "off - choose to turn on"),
                ui::row("2", "Pair a keyboard or remote", LV_SYMBOL_PLUS, bt_on() ? "" : "turn Bluetooth on first",
                        ui::Mark::More),
            };
            std::vector<bt_store::Device> devices = bt_store::devices();
            for (size_t i = 0; i < devices.size(); i++) {
                bool here = bt_hid::is_connected(devices[i]);
                p.rows.push_back(ui::row(std::to_string(i + 3), bt_name(devices[i].name), LV_SYMBOL_KEYBOARD,
                                         std::string(here ? "connected" : "paired") + " - choose to forget"));
            }
            p.rows.push_back(ui::back_row());
            break;
        }

        case Screen::BtScan: {
            p.title = "Pair a device";
            if (!s_bt_scanned) {
                p.info = "Looking for keyboards and remotes... Put yours in pairing mode now (see its manual).";
                p.busy = true;
                break;
            }
            p.info = s_bt_found.empty() ? "No keyboard or remote found. Is it in pairing mode?"
                                        : "Strongest first. Pick yours:";
            for (size_t i = 0; i < s_bt_found.size(); i++) {
                const bt_hid::Found& f = s_bt_found[i];
                p.rows.push_back(ui::row(std::to_string(i + 1), bt_name(f.name), LV_SYMBOL_BLUETOOTH,
                                         std::string(ui::signal_words(f.rssi)) + ", " + bt_address(f.addr)));
            }
            p.rows.push_back(ui::row(std::to_string(s_bt_found.size() + 1), "Look again", LV_SYMBOL_REFRESH));
            p.rows.push_back(ui::back_row());
            break;
        }

        case Screen::BtForgetConfirm:
            p.title = "Forget device";
            p.info = "Forget " + bt_name(s_bt_forget.name) + "? To use it again, pair it again.";
            p.rows = {ui::row("y", "Forget it", LV_SYMBOL_TRASH), ui::row("n", "Keep it", LV_SYMBOL_CLOSE)};
            p.rows[0].tone = ui::Tone::Danger;
            p.selected = 1;
            break;

        case Screen::PortalPage:
        case Screen::PortalField:
        case Screen::PortalOption:
            build_portal_page(p);
            break;

        case Screen::Music:
        case Screen::MusicSongs:
        case Screen::MusicGroups:
        case Screen::MusicSearch:
        case Screen::MusicSettings:
        case Screen::MusicFolder:
            build_music_page(p);
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
// Bluetooth
// ---------------------------------------------------------------------------

// Turns Bluetooth on or off (worker: NimBLE start / stop block); remembered for the next boot
static void bt_switch(bool on)
{
    auto* err = new esp_err_t(ESP_OK);
    bool started = run_job(
        [=]() {
            if (on) {
                *err = bt_hid::start();
            } else {
                bt_hid::stop();
            }
        },
        [=]() {
            s_bt_switching = false;
            bt_store::set_enabled(on && *err == ESP_OK);
            if (on && *err != ESP_OK) {
                s_notice.add("Bluetooth didn't start.\n");
            }
            delete err;
            if (s_screen == Screen::Bluetooth) {
                refresh();
            } else {
                update_status();
            }
        });
    if (started) {
        s_bt_switching = true;
    } else {
        delete err;
    }
}

static void bt_scan()
{
    s_bt_scanned = false;
    s_bt_found.clear();
    go(Screen::BtScan);
    auto* list = new std::vector<bt_hid::Found>();
    auto* err = new esp_err_t(ESP_FAIL);
    uint32_t gen = s_generation;
    bool started = run_job([=]() { *err = bt_hid::scan(*list); },
                           [=]() {
                               if (gen == s_generation && s_screen == Screen::BtScan) {
                                   if (*err != ESP_OK) {
                                       s_notice.add("Bluetooth scan failed - try again.\n");
                                   }
                                   s_bt_found = *list;
                                   s_bt_scanned = true;
                                   render();
                               }
                               delete list;
                               delete err;
                           });
    if (!started) {
        delete list;
        delete err;
        s_bt_scanned = true;
        render();
    }
}

static void bt_pair(const bt_hid::Found& device)
{
    bt_hid::Found* d = new bt_hid::Found(device);
    bool started = run_job([d]() { bt_hid::pair(*d); }, [d]() { delete d; });
    if (!started) {
        delete d;
    }
    go(Screen::Bluetooth);   // Shows the progress (and the code to type)
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
                    auto join = []() {
                        if (wifi::saved().empty()) {
                            s_notice.add("Welcome! Pick your WiFi network to start listening.\n");
                            scan_networks();
                        } else {
                            auto_connect(false);
                        }
                    };
                    if (music::settings().music_last) {
                        music_resume(join);   // Music played last: it plays again (no WiFi needed)
                    } else {
                        join();
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
                if (!music::loaded() && !s_music_opening) {
                    music_open(false, false, []() { go(Screen::Music); });
                } else {
                    go(Screen::Music);
                }
            } else if (key == "3") {
                go(Screen::Wifi);
            } else if (key == "4") {
                go(Screen::Search);
            } else if (key == "5") {
                if (!s_link_up) {
                    s_notice.add("Connect to a network first.\n");
                    render();
                } else {
                    portal_wipe();
                    portal_load(NULL, false, true);
                }
            } else if (key == "6") {
                go(Screen::Bluetooth);
            } else if (key == "7") {
                go(Screen::About);
            }
            break;

        case Screen::Bluetooth: {
            std::vector<bt_store::Device> devices = bt_store::devices();
            if (key == "1") {
                if (!s_bt_switching) {
                    bt_switch(!bt_on());
                }
                render();
            } else if (key == "2") {
                if (!bt_on() || s_bt_switching) {
                    s_notice.add("Turn Bluetooth on first.\n");
                    render();
                } else {
                    bt_scan();
                }
            } else if (n >= 3 && n < 3 + (int)devices.size()) {
                s_bt_forget = devices[n - 3];
                go(Screen::BtForgetConfirm);
            }
            break;
        }

        case Screen::BtScan: {
            int count = (int)s_bt_found.size();
            if (!s_bt_scanned) {
                break;
            } else if (n >= 1 && n <= count) {
                bt_pair(s_bt_found[n - 1]);
            } else if (n == count + 1) {
                bt_scan();
            }
            break;
        }

        case Screen::BtForgetConfirm:
            if (key == "y") {
                bt_hid::forget(s_bt_forget);
                s_notice.add(("Forgot " + bt_name(s_bt_forget.name) + ".\n").c_str());
                go(Screen::Bluetooth);
            } else if (key == "n") {
                go(Screen::Bluetooth);
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

        case Screen::Music:
        case Screen::MusicSongs:
        case Screen::MusicGroups:
        case Screen::MusicSettings:
        case Screen::MusicFolder:
            music_answer(key, n);
            break;

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

        case Screen::MusicSearch: {
            if (text.empty()) {
                back();
                break;
            }
            music::Indexes found = music::library().search(text);
            if (found.empty()) {
                s_notice.add(("No songs match \"" + portal::printable(text, 40) + "\".\n").c_str());
                render();
            } else {
                show_songs("Search: " + portal::printable(text, 30), found, Screen::Music);
            }
            break;
        }

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
        case Screen::Bluetooth:
        case Screen::Music:
            go(Screen::Menu);
            break;
        case Screen::MusicSongs:
            go(s_songs_back);
            break;
        case Screen::MusicGroups:
        case Screen::MusicSearch:
        case Screen::MusicSettings:
            go(Screen::Music);
            break;
        case Screen::MusicFolder:
            go(Screen::MusicSettings);
            break;
        case Screen::BtScan:
        case Screen::BtForgetConfirm:
            go(Screen::Bluetooth);
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

// Pause / Play: both wait on the stream's tasks, so they run past the display lock
static void toggle_pause()
{
    if (player_paused()) {
        defer([]() { player_play(); });
    } else {
        defer([]() { player_pause(); });
    }
}

// Media keys (Bluetooth keyboards and remotes) work on every screen once the radio is up
static bool media_key(uint32_t key)
{
    switch (key) {
        case KEY_PLAY_PAUSE: toggle_pause(); break;
        case KEY_NEXT_TRACK: player_step(1); break;
        case KEY_PREV_TRACK: player_step(-1); break;
        case KEY_VOL_UP: player_set_volume(player_volume() + 5); break;
        case KEY_VOL_DOWN: player_set_volume(player_volume() - 5); break;
        case KEY_MUTE: player_set_muted(!player_muted()); break;
        default: return false;
    }
    return true;
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
        if (music_active()) {
            const music::Queue& q = music::queue();
            if (n >= 1 && n <= q.size()) {
                music::Indexes songs = q.songs();
                play_songs(songs, n - 1);
                return;
            }
        } else if (n >= 1 && n <= player_count()) {
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
        case ' ': case 'p': case 'P': toggle_pause(); break;
        case 'm': case 'M': player_set_muted(!player_muted()); break;
        case 'f': case 'F':
            if (music::playing() >= 0) {
                music::toggle_favorite(music::playing());
            }
            break;
        case KEY_ENTER: case 's': case 'S': open_list(); return;
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
    if (s_booted && media_key(key)) {
        if (s_screen == Screen::NowPlaying) {
            refresh_now_playing();
        } else {
            update_status();
        }
        return;
    }
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
        case NP_PAUSE: toggle_pause(); break;
        case NP_NEXT: player_step(1); break;
        case NP_MUTE: player_set_muted(!player_muted()); break;
        case NP_VOL_DOWN: player_set_volume(player_volume() - 5); break;
        case NP_VOL_UP: player_set_volume(player_volume() + 5); break;
        case NP_STATIONS: open_list(); return;
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
// The next key from the device's keyboard, else from a Bluetooth keyboard or remote
static uint32_t next_key()
{
    uint32_t key = board_read_key();
    return key ? key : bt_hid::read_key();
}

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
    // Music: the scan's progress on "Please wait", then genres looked up in the background
    if (s_music_opening && s_screen == Screen::Busy && music::scan_progress() != s_music_scanned_shown) {
        s_music_scanned_shown = music::scan_progress();
        s_busy_text = "Reading your music folder... " + plural(s_music_scanned_shown, "song", "songs") + " so far.";
        render();
    }
    bool typing = s_screen == Screen::WifiPassword || s_screen == Screen::WifiHidden || s_screen == Screen::PortalField ||
                  s_screen == Screen::Busy;
    if (!typing && !s_loading_stations) {
        lookup_next_genre();
    }
    // Bluetooth: results to show, the page to refresh, and starting it once the radio is up
    uint32_t bv = bt_hid::version();
    if (bv != s_bt_version) {
        s_bt_version = bv;
        std::string m = bt_hid::take_message();
        if (!m.empty()) {
            s_notice.add(m.c_str());
        }
        if (s_screen == Screen::Bluetooth || (!m.empty() && s_screen != Screen::NowPlaying && s_screen != Screen::BtScan)) {
            refresh();
        } else {
            update_status();
        }
    }
    if (s_bt_autostart && s_booted && !s_busy && !s_loading_stations && s_screen != Screen::Busy) {
        s_bt_autostart = false;
        bt_switch(true);
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
    char crash[160];
    crash_note_take(crash, sizeof(crash));
    if (crash[0]) {
        s_last_restart = crash;
        s_notice.add((s_last_restart + "\n").c_str());
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
    player_set_hold(music::settings().music_last);   // Music resumes at boot rather than the radio
    wifi::on_link(on_link);
    s_bt_autostart = bt_store::enabled();   // Started once WiFi is up (Tab5: the C6 link)

    if (lvgl_port_lock(0)) {
        boot_start();
        lvgl_port_unlock();
    }

    for (;;) {
        Event ev;
        bool got = xQueueReceive(s_events, &ev, pdMS_TO_TICKS(20)) == pdTRUE;
        uint32_t key = next_key();
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
                key = next_key();
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
