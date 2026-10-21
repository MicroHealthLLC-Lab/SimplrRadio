/*
 * Music player (music_player.h): the library on the SD card, its index files, the play queue
 * and the decoder (music_decoder.c) behind the player's controls.
 *
 * Files on the card
 *   <card>/.simplrradio/index.bin    the library of the chosen folder (music_library.hpp)
 *   <card>/.simplrradio/genres.bin   genres looked up online, per artist (kept across scans)
 * Both are checked when read (they are on removable media); a bad one is rebuilt.
 */

#include "music_player.h"
#include "board.h"
#include "music_decoder.h"
#include "player.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

static const char* TAG = "music";

#define SONG_DEBOUNCE_MS  300    // Paging through songs: the decoder starts once the user pauses
#define SKIP_AFTER_MS     1500   // A song that can't be played shows why, then the next one plays

namespace music
{
    void* big_alloc(size_t bytes)
    {
        return heap_caps_malloc(bytes ? bytes : 1, MALLOC_CAP_SPIRAM);
    }

    void big_free(void* p)
    {
        heap_caps_free(p);
    }
}

using namespace music;

static Library s_lib;
static GenreCache s_genres;
static Settings s_settings;
static bool s_settings_read;
static std::vector<uint32_t> s_favorites;
static bool s_loaded;
static Queue s_queue;
static volatile int s_progress;

static bool s_active;
static bool s_paused;
static bool s_pending;
static TickType_t s_pending_tick;
static TickType_t s_failed_at;
static int s_failures;
static player_status_t s_status = PLAYER_IDLE;
static volatile uint32_t s_version;

static void changed()
{
    s_version = s_version + 1;
}

static std::string card_dir(const std::string& folder)
{
    std::string base = board_sd_root();
    return folder.empty() || folder == "/" ? base : base + folder;
}

static std::string index_dir()
{
    return std::string(board_sd_root()) + "/.simplrradio";
}

static std::string song_path(const Track& t)
{
    return card_dir(s_settings.folder) + "/" + t.path;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace music
{

Settings& settings()
{
    if (!s_settings_read) {
        s_settings = load_settings();
        s_favorites = load_favorites();
        s_settings_read = true;
    }
    return s_settings;
}

Library& library() { return s_lib; }
GenreCache& genres() { return s_genres; }
std::vector<uint32_t>& favorites()
{
    settings();
    return s_favorites;
}
bool loaded() { return s_loaded; }
const Queue& queue() { return s_queue; }

std::string folder_shown()
{
    const std::string& f = settings().folder;
    return f.empty() || f == "/" ? "the whole card" : f;
}

// ---------------------------------------------------------------------------
// The card (worker)
// ---------------------------------------------------------------------------

esp_err_t mount()
{
    return board_sd_mount();
}

struct FileCtx {
    FILE* f;
    uint64_t pos;
};

static size_t read_at(void* ctx, uint64_t offset, uint8_t* buf, size_t len)
{
    FileCtx* c = static_cast<FileCtx*>(ctx);
    if (offset > 0x7FFFFFFF) {
        return 0;
    }
    if (c->pos != offset) {
        if (fseek(c->f, (long)offset, SEEK_SET) != 0) {
            return 0;
        }
        c->pos = offset;
    }
    size_t n = fread(buf, 1, len, c->f);
    c->pos += n;
    return n;
}

// One file's tags into t; false if it isn't the audio its name says
static bool read_song(const std::string& full, const std::string& rel, media_format_t format, const GenreCache& cache,
                      Track& t)
{
    FILE* f = fopen(full.c_str(), "rb");   // noopengrep: mh.gitlab.c_misc_rule-fopen-open - FAT on the SD card has no links, owners or permissions to swap between a check and the open; the radio only reads songs and writes its own index
    if (!f) {
        return false;
    }
    struct stat st;
    uint64_t size = stat(full.c_str(), &st) == 0 && st.st_size > 0 ? (uint64_t)st.st_size : 0;
    FileCtx ctx = {f, 0};
    media_file_t file = {read_at, &ctx, size};
    media_tags_t tags;
    bool ok = size > 0 && media_tags_read(&file, format, &tags);
    fclose(f);
    if (!ok) {
        return false;
    }
    media_tags_from_path(rel.c_str(), &tags);
    if (!tags.genre[0] && tags.artist[0]) {
        snprintf(tags.genre, sizeof(tags.genre), "%s", cache.genre(tags.artist).c_str());
    }
    static const Track EMPTY = {};
    t = EMPTY;
    snprintf(t.path, sizeof(t.path), "%s", rel.c_str());
    snprintf(t.title, sizeof(t.title), "%s", tags.title);
    snprintf(t.artist, sizeof(t.artist), "%s", tags.artist);
    snprintf(t.album, sizeof(t.album), "%s", tags.album);
    snprintf(t.genre, sizeof(t.genre), "%s", tags.genre);
    t.duration_ms = tags.duration_ms;
    t.audio_offset = tags.audio_offset;
    t.year = tags.year;
    t.track = tags.track;
    t.format = (uint8_t)tags.format;
    t.flags = tags.playable ? 0 : UNPLAYABLE;
    return true;
}

// Folders that hold no music: the system's, and anything hidden (ours included)
static bool skipped(const char* name)
{
    return name[0] == '.' || strcasecmp(name, "System Volume Information") == 0 ||
           strcasecmp(name, "$RECYCLE.BIN") == 0 || strcasecmp(name, "LOST.DIR") == 0;
}

static bool is_dir(const std::string& path, const struct dirent* e)
{
    if (e->d_type == DT_DIR) {
        return true;
    }
    if (e->d_type != DT_UNKNOWN) {
        return false;
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

ScanResult scan(const std::string& folder, Library& into, const GenreCache& cache)
{
    ScanResult r;
    into.clear();
    s_progress = 0;
    std::string base = card_dir(folder);
    struct Pending {
        std::string rel;
        int depth = 0;
    };
    std::vector<Pending> todo = {{"", 0}};
    while (!todo.empty() && !r.full) {
        Pending at = todo.back();
        todo.pop_back();
        std::string dir = at.rel.empty() ? base : base + "/" + at.rel;
        DIR* d = opendir(dir.c_str());
        if (!d) {
            continue;
        }
        r.card = true;
        std::vector<std::string> folders;
        while (struct dirent* e = readdir(d)) {
            const char* name = e->d_name;
            if (skipped(name)) {
                continue;
            }
            std::string rel = at.rel.empty() ? std::string(name) : at.rel + "/" + name;
            std::string full = dir + "/" + name;
            if (is_dir(full, e)) {
                if (at.depth + 1 > MAX_FOLDER_DEPTH) {
                    r.too_long++;
                } else {
                    folders.push_back(rel);
                }
                continue;
            }
            media_format_t format = media_format_from_name(name);
            if (format == MEDIA_NONE) {
                continue;
            }
            if (rel.size() >= PATH_LEN) {
                r.too_long++;
                continue;
            }
            Track t;
            if (!read_song(full, rel, format, cache, t)) {
                r.unreadable++;
                continue;
            }
            Track* slot = into.add();
            if (!slot) {
                r.full = true;
                break;
            }
            *slot = t;
            r.songs++;
            s_progress = r.songs;
        }
        closedir(d);
        // Depth first, in the order the card lists them
        for (auto it = folders.rbegin(); it != folders.rend(); ++it) {
            todo.push_back({*it, at.depth + 1});
        }
    }
    ESP_LOGI(TAG, "Scan: %d songs, %d unreadable, %d skipped", r.songs, r.unreadable, r.too_long);
    return r;
}

int scan_progress()
{
    return s_progress;
}

static bool write_file(const std::string& path, const std::function<bool(const std::function<bool(const void*, size_t)>&)>& body)
{
    mkdir(index_dir().c_str(), 0775);
    std::string tmp = path + ".new";
    FILE* f = fopen(tmp.c_str(), "wb");   // noopengrep: mh.gitlab.c_misc_rule-fopen-open - FAT on the SD card has no links, owners or permissions to swap between a check and the open; the radio only reads songs and writes its own index
    if (!f) {
        return false;
    }
    bool ok = body([f](const void* data, size_t len) { return fwrite(data, 1, len, f) == len; });
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path.c_str());
        ok = rename(tmp.c_str(), path.c_str()) == 0;
    }
    if (!ok) {
        remove(tmp.c_str());
    }
    return ok;
}

static bool read_file(const std::string& path, const std::function<bool(const std::function<size_t(void*, size_t)>&)>& body)
{
    FILE* f = fopen(path.c_str(), "rb");   // noopengrep: mh.gitlab.c_misc_rule-fopen-open - FAT on the SD card has no links, owners or permissions to swap between a check and the open; the radio only reads songs and writes its own index
    if (!f) {
        return false;
    }
    bool ok = body([f](void* data, size_t len) { return fread(data, 1, len, f); });
    fclose(f);
    return ok;
}

bool save_index(const std::string& folder, const Library& lib)
{
    return write_file(index_dir() + "/index.bin", [&](const std::function<bool(const void*, size_t)>& w) {
        return lib.save(folder, w);
    });
}

bool load_index(const std::string& folder, Library& into)
{
    return into.reserve() && read_file(index_dir() + "/index.bin", [&](const std::function<size_t(void*, size_t)>& r) {
        return into.load(folder, r);
    });
}

bool save_genres(const GenreCache& cache)
{
    return write_file(index_dir() + "/genres.bin", [&](const std::function<bool(const void*, size_t)>& w) {
        return cache.save(w);
    });
}

bool load_genres(GenreCache& into)
{
    return read_file(index_dir() + "/genres.bin", [&](const std::function<size_t(void*, size_t)>& r) {
        return into.load(r);
    });
}

std::vector<std::string> list_folders(const std::string& folder, bool& ok)
{
    std::vector<std::string> out;
    std::string dir = card_dir(folder);
    DIR* d = opendir(dir.c_str());
    ok = d != nullptr;
    if (!d) {
        return out;
    }
    while (struct dirent* e = readdir(d)) {
        if (!skipped(e->d_name) && is_dir(dir + "/" + e->d_name, e) &&
            folder.size() + 1 + strnlen(e->d_name, ROOT_LEN) < ROOT_LEN) {
            out.push_back(e->d_name);
            if ((int)out.size() >= MAX_FOLDERS) {
                break;
            }
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    return out;
}

// ---------------------------------------------------------------------------
// The library and the queue (app task)
// ---------------------------------------------------------------------------

void install(Library& built)
{
    s_queue.remap(s_lib, built);
    s_lib.swap(built);
    s_loaded = true;
    changed();
}

static void remember(const Track& t)
{
    Settings& s = settings();
    uint32_t hash = path_hash(t.path);
    if (!s.music_last || s.last_song != hash) {
        s.music_last = true;
        s.last_song = hash;
        save_settings(s);   // Written only when it changes
    }
}

static void commit()
{
    s_pending = false;
    s_paused = false;
    music_decoder_stop();
    int i = s_queue.current();
    if (i < 0 || i >= s_lib.size()) {
        s_status = PLAYER_IDLE;
        changed();
        return;
    }
    const Track& t = s_lib.at(i);
    if (t.flags & UNPLAYABLE) {
        s_status = PLAYER_UNAVAILABLE;
        s_failed_at = xTaskGetTickCount();
        changed();
        return;
    }
    s_status = PLAYER_CONNECTING;
    music_decoder_start(song_path(t).c_str(), (media_format_t)t.format, t.audio_offset);
    remember(t);
    changed();
}

void play(const Indexes& songs, int start)
{
    if (songs.empty()) {
        return;
    }
    player_release();   // The radio's stream stops; its station stays chosen
    s_active = true;
    s_failures = 0;
    s_queue.set(songs, start, settings().shuffle, esp_random());
    commit();
}

int playing()
{
    return s_active ? s_queue.current() : -1;
}

uint32_t elapsed_ms()
{
    return s_active && !s_pending ? music_decoder_elapsed_ms() : 0;
}

void set_shuffle(bool on)
{
    settings().shuffle = on;
    save_settings(s_settings);
    s_queue.shuffle(on, esp_random());
    changed();
}

void set_repeat(Repeat r)
{
    settings().repeat = r;
    save_settings(s_settings);
    changed();
}

bool is_favorite(int song)
{
    return song >= 0 && song < s_lib.size() && music::is_favorite(favorites(), path_hash(s_lib.at(song).path));
}

bool toggle_favorite(int song)
{
    if (song < 0 || song >= s_lib.size()) {
        return false;
    }
    if (!music::toggle_favorite(favorites(), path_hash(s_lib.at(song).path))) {
        return false;
    }
    save_favorites(s_favorites);
    changed();
    return true;
}

}   // namespace music

// ---------------------------------------------------------------------------
// For player.c
// ---------------------------------------------------------------------------

extern "C" bool music_active(void)
{
    return s_active;
}

extern "C" void music_leave(void)
{
    if (!s_active) {
        return;
    }
    music_decoder_stop();
    s_active = false;
    s_paused = false;
    s_pending = false;
    s_status = PLAYER_IDLE;
    Settings& s = settings();
    if (s.music_last) {
        s.music_last = false;
        save_settings(s);
    }
    changed();
}

extern "C" void music_step(int delta)
{
    if (!s_active || s_queue.empty()) {
        return;
    }
    s_queue.step(delta, settings().repeat, false);
    s_pending = true;
    s_pending_tick = xTaskGetTickCount();
    s_paused = false;
    s_failures = 0;
    s_status = PLAYER_CONNECTING;
    changed();
}

extern "C" void music_pause(void)
{
    if (!s_active || s_paused) {
        return;
    }
    s_paused = true;
    music_decoder_pause(true);
    s_status = PLAYER_PAUSED;
    changed();
}

extern "C" void music_play(void)
{
    if (!s_active) {
        return;
    }
    if (s_paused && music_decoder_running()) {
        s_paused = false;
        music_decoder_pause(false);
        s_status = PLAYER_PLAYING;
        changed();
        return;
    }
    if (s_status == PLAYER_IDLE && s_queue.current() < 0 && !s_queue.empty()) {
        s_queue.jump(0);
    }
    s_failures = 0;
    commit();   // Paused at the end of a song, or the list had played through: start it again
}

extern "C" bool music_paused(void)
{
    return s_active && s_paused;
}

extern "C" void music_tick(void)
{
    if (!s_active) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if (s_pending) {
        if (now - s_pending_tick >= pdMS_TO_TICKS(SONG_DEBOUNCE_MS)) {
            commit();
        }
        return;
    }
    if (s_paused || s_status == PLAYER_IDLE) {
        return;
    }
    Repeat repeat = settings().repeat;
    if (s_status == PLAYER_UNAVAILABLE) {
        if (now - s_failed_at < pdMS_TO_TICKS(SKIP_AFTER_MS)) {
            return;
        }
        // On to the next song (never the same one again, whatever the repeat)
        if (++s_failures >= s_queue.size() ||
            !s_queue.step(1, repeat == Repeat::Off ? Repeat::Off : Repeat::All, true)) {
            s_status = PLAYER_IDLE;
            changed();
            return;
        }
        commit();
        return;
    }
    if (music_decoder_failed()) {
        ESP_LOGW(TAG, "Song can't be played");
        s_status = PLAYER_UNAVAILABLE;
        s_failed_at = now;
        changed();
        return;
    }
    if (music_decoder_finished()) {
        s_failures = 0;
        if (s_queue.step(1, repeat, true)) {
            commit();
        } else {
            s_status = PLAYER_IDLE;   // The list played through
            changed();
        }
        return;
    }
    player_status_t status = music_decoder_has_audio() ? PLAYER_PLAYING : PLAYER_CONNECTING;
    if (status != s_status) {
        s_status = status;
        changed();
    }
}

extern "C" int music_status(void)
{
    return s_status;
}

extern "C" uint32_t music_version(void)
{
    // The elapsed time counts too: Now Playing shows it to the second
    return s_version * 7919u + (s_active ? music_decoder_elapsed_ms() / 1000 : 0);
}
