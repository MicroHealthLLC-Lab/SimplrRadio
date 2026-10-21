/*
 * Music library (music_library.hpp).
 */

#include "music_library.hpp"
#include "settings_nvs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace music
{

static const char* NS = "music";

static const char INDEX_MAGIC[4] = {'S', 'R', 'M', 'I'};
static const char GENRE_MAGIC[4] = {'S', 'R', 'M', 'G'};
static const uint32_t FILE_VERSION = 1;

// Header of both index files; the records and a CRC-32 of header and records follow. Written
// and read by the same firmware (byte order and record layout are the radio's own).
struct FileHeader {
    char magic[4];
    uint32_t version;
    uint32_t record_size;
    uint32_t count;
    char root[ROOT_LEN];   // The music folder the index was made for ("" in the genre file)
};

uint32_t crc32(uint32_t crc, const void* data, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t path_hash(const char* path)
{
    uint32_t h = 2166136261u;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; p++) {
        h = (h ^ *p) * 16777619u;
    }
    return h ? h : 1;   // 0 means "none" in the settings
}

std::string duration_text(uint32_t ms)
{
    if (ms == 0) {
        return "";
    }
    uint32_t s = ms / 1000;
    char text[16];
    if (s >= 3600) {
        snprintf(text, sizeof(text), "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    } else {
        snprintf(text, sizeof(text), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    }
    return text;
}

// ---------------------------------------------------------------------------
// Groups and order
// ---------------------------------------------------------------------------

static const char* field(const Track& t, Group g)
{
    return g == Group::Artist ? t.artist : g == Group::Album ? t.album : t.genre;
}

std::string group_name(const Track& t, Group g)
{
    const char* name = field(t, g);
    if (name[0]) {
        return name;
    }
    return g == Group::Artist ? "Unknown artist" : g == Group::Album ? "Unknown album" : "Unknown genre";
}

// Names compared without case; an empty one (unknown) after every other
static int name_cmp(const char* a, const char* b)
{
    if (!a[0] || !b[0]) {
        return (a[0] ? 0 : 1) - (b[0] ? 0 : 1);
    }
    return strcasecmp(a, b);
}

// The folder part of a path ("Artist/Album" of "Artist/Album/01.mp3")
static size_t folder_len(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash ? (size_t)(slash - path) : 0;
}

static int folder_cmp(const Track& a, const Track& b)
{
    size_t la = folder_len(a.path), lb = folder_len(b.path);
    int c = strncmp(a.path, b.path, std::min(la, lb));
    return c ? c : (int)la - (int)lb;
}

// Album, then track number, then title: an album's songs in order
static int album_order(const Track& a, const Track& b)
{
    int c = name_cmp(a.album, b.album);
    if (!c) {
        c = folder_cmp(a, b);
    }
    if (!c) {
        c = (a.track ? a.track : 10000) - (b.track ? b.track : 10000);
    }
    if (!c) {
        c = name_cmp(a.title, b.title);
    }
    return c ? c : strcmp(a.path, b.path);
}

int Library::compare(Group g, const Track& a, const Track& b) const
{
    int c = 0;
    if (g == Group::Genre) {
        c = name_cmp(a.genre, b.genre);
    }
    if (!c && g != Group::Album) {
        c = name_cmp(a.artist, b.artist);
    }
    return c ? c : album_order(a, b);
}

bool Library::same_group(Group g, const Track& a, const Track& b) const
{
    if (g == Group::Album) {
        return name_cmp(a.album, b.album) == 0 && folder_cmp(a, b) == 0;
    }
    return name_cmp(field(a, g), field(b, g)) == 0;
}

Library::~Library()
{
    big_free(tracks);
}

bool Library::reserve()
{
    if (!tracks) {
        tracks = static_cast<Track*>(big_alloc(sizeof(Track) * MAX_TRACKS));
    }
    count = 0;
    return tracks != nullptr;
}

void Library::swap(Library& other)
{
    std::swap(tracks, other.tracks);
    std::swap(count, other.count);
}

Track* Library::add()
{
    if (!tracks || count >= MAX_TRACKS) {
        return nullptr;
    }
    Track* t = &tracks[count++];
    static const Track EMPTY = {};
    *t = EMPTY;
    return t;
}

Indexes Library::all() const
{
    Indexes out;
    out.reserve(count);
    for (int i = 0; i < count; i++) {
        out.push_back((uint16_t)i);
    }
    std::sort(out.begin(), out.end(), [this](uint16_t a, uint16_t b) {
        return compare(Group::Artist, tracks[a], tracks[b]) < 0;
    });
    return out;
}

Indexes Library::in_group(Group g, int any) const
{
    Indexes out;
    if (any < 0 || any >= count) {
        return out;
    }
    const Track& ref = tracks[any];
    for (int i = 0; i < count; i++) {
        if (same_group(g, tracks[i], ref)) {
            out.push_back((uint16_t)i);
        }
    }
    std::sort(out.begin(), out.end(), [this, g](uint16_t a, uint16_t b) {
        return (g == Group::Album ? album_order(tracks[a], tracks[b]) : compare(g, tracks[a], tracks[b])) < 0;
    });
    return out;
}

Groups Library::groups(Group g) const
{
    Indexes order;
    order.reserve(count);
    for (int i = 0; i < count; i++) {
        order.push_back((uint16_t)i);
    }
    std::sort(order.begin(), order.end(), [this, g](uint16_t a, uint16_t b) {
        int c = name_cmp(field(tracks[a], g), field(tracks[b], g));
        if (!c && g == Group::Album) {
            c = folder_cmp(tracks[a], tracks[b]);
        }
        return (c ? c : (int)a - (int)b) < 0;
    });
    Groups out;
    for (uint16_t i : order) {
        if (!out.empty() && same_group(g, tracks[out.back().first], tracks[i])) {
            out.back().count++;
        } else {
            out.push_back(GroupEntry{i, 1});
        }
    }
    return out;
}

static std::string lower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

Indexes Library::search(const std::string& words) const
{
    std::vector<std::string> wanted;
    std::string w;
    for (char c : lower(words) + " ") {
        if (c == ' ') {
            if (!w.empty()) {
                wanted.push_back(w);
            }
            w.clear();
        } else {
            w += c;
        }
    }
    Indexes out;
    if (wanted.empty()) {
        return out;
    }
    for (uint16_t i : all()) {
        const Track& t = tracks[i];
        std::string text = lower(std::string(t.title) + " " + t.artist + " " + t.album);
        bool every = true;
        for (const auto& word : wanted) {
            every = every && text.find(word) != std::string::npos;
        }
        if (every) {
            out.push_back(i);
        }
    }
    return out;
}

Indexes Library::with_hashes(const std::vector<uint32_t>& hashes) const
{
    Indexes out;
    for (uint16_t i : all()) {
        if (std::find(hashes.begin(), hashes.end(), path_hash(tracks[i].path)) != hashes.end()) {
            out.push_back(i);
        }
    }
    return out;
}

int Library::find(const char* path) const
{
    for (int i = 0; i < count; i++) {
        if (strcmp(tracks[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

int Library::find_hash(uint32_t hash) const
{
    for (int i = 0; hash && i < count; i++) {
        if (path_hash(tracks[i].path) == hash) {
            return i;
        }
    }
    return -1;
}

std::vector<std::string> Library::artists_without_genre(size_t max, const std::function<bool(const char*)>& skip) const
{
    std::vector<std::string> out;
    for (int i = 0; i < count && out.size() < max; i++) {
        const Track& t = tracks[i];
        if (t.genre[0] || !t.artist[0] || (skip && skip(t.artist))) {
            continue;
        }
        bool seen = false;
        for (const auto& a : out) {
            seen = seen || strcasecmp(a.c_str(), t.artist) == 0;
        }
        if (!seen) {
            out.push_back(t.artist);
        }
    }
    return out;
}

int Library::next_without_genre(int from, const std::function<bool(const char*)>& skip) const
{
    for (int i = from < 0 ? 0 : from; i < count; i++) {
        const Track& t = tracks[i];
        if (!t.genre[0] && t.artist[0] && !(skip && skip(t.artist))) {
            return i;
        }
    }
    return -1;
}

int Library::set_artist_genre(const std::string& artist, const std::string& genre)
{
    int changed = 0;
    for (int i = 0; i < count && !genre.empty(); i++) {
        Track& t = tracks[i];
        if (!t.genre[0] && strcasecmp(t.artist, artist.c_str()) == 0) {
            snprintf(t.genre, sizeof(t.genre), "%s", genre.c_str());
            t.flags |= GENRE_ONLINE;
            changed++;
        }
    }
    return changed;
}

int Library::set_artist_name(const std::string& from, const std::string& to)
{
    int changed = 0;
    if (to.empty() || to.size() >= MEDIA_NAME_LEN || strcasecmp(from.c_str(), to.c_str()) != 0) {
        return 0;   // Only the capitals change: another name could be another artist
    }
    for (int i = 0; i < count; i++) {
        Track& t = tracks[i];
        if (strcasecmp(t.artist, from.c_str()) == 0 && strcmp(t.artist, to.c_str()) != 0) {
            snprintf(t.artist, sizeof(t.artist), "%s", to.c_str());
            changed++;
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Index files
// ---------------------------------------------------------------------------

// Text from the card made printable and terminated (a written index is already ASCII)
template <size_t N> static void printable(char (&text)[N])
{
    text[N - 1] = '\0';
    for (size_t i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c > 0x7E) {
            text[i] = '?';
        }
    }
}

// A path from the index: terminated, relative, without ".." (it is opened under the folder)
static bool safe_path(char (&path)[PATH_LEN])
{
    path[PATH_LEN - 1] = '\0';
    if (!path[0] || path[0] == '/') {
        return false;
    }
    for (const char* p = path; *p; p++) {
        if ((unsigned char)*p < 0x20) {
            return false;
        }
    }
    std::string s = std::string("/") + path + "/";
    return s.find("/../") == std::string::npos && s.find("/./") == std::string::npos;
}

static FileHeader header(const char magic[4], uint32_t record_size, uint32_t count, const std::string& root)
{
    FileHeader h = {};
    std::copy(magic, magic + 4, h.magic);
    h.version = FILE_VERSION;
    h.record_size = record_size;
    h.count = count;
    snprintf(h.root, sizeof(h.root), "%s", root.c_str());
    return h;
}

static bool write_file(const FileHeader& h, const void* records, size_t bytes,
                       const std::function<bool(const void*, size_t)>& write)
{
    uint32_t crc = crc32(0, &h, sizeof(h));
    crc = crc32(crc, records, bytes);
    return write(&h, sizeof(h)) && (bytes == 0 || write(records, bytes)) && write(&crc, sizeof(crc));
}

// Reads exactly len bytes
static bool read_all(const std::function<size_t(void*, size_t)>& read, void* buf, size_t len)
{
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (len) {
        size_t n = read(p, len);
        if (n == 0 || n > len) {
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

// The header, checked; false if it isn't this kind of file
static bool read_header(const std::function<size_t(void*, size_t)>& read, const char magic[4], uint32_t record_size,
                        uint32_t max, FileHeader& h)
{
    if (!read_all(read, &h, sizeof(h))) {
        return false;
    }
    h.root[ROOT_LEN - 1] = '\0';
    return std::equal(magic, magic + 4, h.magic) && h.version == FILE_VERSION && h.record_size == record_size &&
           h.count <= max;
}

bool Library::save(const std::string& root, const std::function<bool(const void*, size_t)>& write) const
{
    FileHeader h = header(INDEX_MAGIC, sizeof(Track), (uint32_t)count, root);
    return write_file(h, tracks, sizeof(Track) * count, write);
}

bool Library::load(const std::string& root, const std::function<size_t(void*, size_t)>& read)
{
    FileHeader h;
    if (!tracks || !read_header(read, INDEX_MAGIC, sizeof(Track), MAX_TRACKS, h) || root != h.root) {
        count = 0;
        return false;
    }
    uint32_t crc = crc32(0, &h, sizeof(h));
    uint32_t stored = 0;
    if (!read_all(read, tracks, sizeof(Track) * h.count) || !read_all(read, &stored, sizeof(stored)) ||
        crc32(crc, tracks, sizeof(Track) * h.count) != stored) {
        count = 0;
        return false;
    }
    count = 0;
    for (uint32_t i = 0; i < h.count; i++) {
        Track t = tracks[i];
        if (!safe_path(t.path) || t.format == MEDIA_NONE || t.format >= MEDIA_FORMATS) {
            continue;   // Dropped, not trusted
        }
        printable(t.title);
        printable(t.artist);
        printable(t.album);
        printable(t.genre);
        tracks[count++] = t;
    }
    return true;
}

int GenreCache::index_of(const std::string& artist) const
{
    for (size_t i = 0; i < entries.size(); i++) {
        if (strcasecmp(entries[i].artist, artist.c_str()) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool GenreCache::known(const std::string& artist) const
{
    return index_of(artist) >= 0;
}

std::string GenreCache::genre(const std::string& artist) const
{
    int i = index_of(artist);
    return i >= 0 ? entries[i].genre : "";
}

void GenreCache::put(const std::string& artist, const std::string& genre)
{
    if (artist.empty()) {
        return;
    }
    int i = index_of(artist);
    if (i < 0) {
        if (entries.size() >= (size_t)MAX_GENRE_CACHE) {
            entries.erase(entries.begin());   // The oldest goes
        }
        entries.push_back(Entry{});
        i = (int)entries.size() - 1;
    }
    snprintf(entries[i].artist, sizeof(entries[i].artist), "%s", artist.c_str());
    snprintf(entries[i].genre, sizeof(entries[i].genre), "%s", genre.c_str());
}

bool GenreCache::save(const std::function<bool(const void*, size_t)>& write) const
{
    FileHeader h = header(GENRE_MAGIC, sizeof(Entry), (uint32_t)entries.size(), "");
    return write_file(h, entries.data(), sizeof(Entry) * entries.size(), write);
}

bool GenreCache::load(const std::function<size_t(void*, size_t)>& read)
{
    entries.clear();
    FileHeader h;
    if (!read_header(read, GENRE_MAGIC, sizeof(Entry), MAX_GENRE_CACHE, h)) {
        return false;
    }
    std::vector<Entry, BigAllocator<Entry>> got(h.count);
    uint32_t stored = 0;
    if (!read_all(read, got.data(), sizeof(Entry) * h.count) || !read_all(read, &stored, sizeof(stored)) ||
        crc32(crc32(0, &h, sizeof(h)), got.data(), sizeof(Entry) * h.count) != stored) {
        return false;
    }
    for (Entry& e : got) {
        printable(e.artist);
        printable(e.genre);
        if (e.artist[0]) {
            entries.push_back(e);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Queue
// ---------------------------------------------------------------------------

static uint32_t next_random(uint32_t& state)
{
    // xorshift32: an even spread is all shuffling needs
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static void shuffle_order(Indexes& order, uint32_t seed)
{
    uint32_t state = seed ? seed : 0x9E3779B9u;
    for (size_t i = order.size(); i > 1; i--) {
        size_t j = next_random(state) % i;
        std::swap(order[i - 1], order[j]);
    }
}

void Queue::clear()
{
    items.clear();
    order.clear();
    pos = -1;
}

void Queue::set(const Indexes& songs, int start, bool shuffle_on, uint32_t seed)
{
    items = songs;
    if (start < 0 || start >= (int)items.size()) {
        start = 0;
    }
    pos = items.empty() ? -1 : start;
    is_shuffled = false;
    order.clear();
    for (size_t i = 0; i < items.size(); i++) {
        order.push_back((uint16_t)i);
    }
    if (shuffle_on) {
        shuffle(true, seed);
    }
}

int Queue::current() const
{
    return pos >= 0 && pos < (int)order.size() ? items[order[pos]] : -1;
}

int Queue::list_position() const
{
    return pos >= 0 && pos < (int)order.size() ? order[pos] : -1;
}

bool Queue::step(int delta, Repeat repeat, bool ended)
{
    int n = (int)order.size();
    if (n == 0) {
        return false;
    }
    if (ended && repeat == Repeat::One) {
        return true;   // The same song again
    }
    int next = pos + delta;
    if (next >= 0 && next < n) {
        pos = next;
        return true;
    }
    if (ended && repeat == Repeat::Off) {
        return false;   // The list played through
    }
    pos = ((next % n) + n) % n;   // Next / Previous by hand always wrap
    return true;
}

void Queue::jump(int list_index)
{
    for (size_t p = 0; p < order.size(); p++) {
        if (order[p] == list_index) {
            pos = (int)p;
            return;
        }
    }
}

void Queue::shuffle(bool on, uint32_t seed)
{
    int cur = list_position();
    order.clear();
    for (size_t i = 0; i < items.size(); i++) {
        order.push_back((uint16_t)i);
    }
    is_shuffled = on;
    if (on) {
        shuffle_order(order, seed);
        // The song playing stays first; the rest follow in the new order
        auto it = std::find(order.begin(), order.end(), (uint16_t)(cur < 0 ? 0 : cur));
        if (it != order.end()) {
            std::rotate(order.begin(), it, it + 1);
        }
        pos = items.empty() ? -1 : 0;
    } else {
        pos = cur < 0 && !items.empty() ? 0 : cur;
    }
}

void Queue::remap(const Library& from, const Library& to)
{
    int cur = current();
    std::string playing = cur >= 0 && cur < from.size() ? from.at(cur).path : "";
    Indexes kept;
    int start = 0;
    for (uint16_t i : items) {
        if (i >= from.size()) {
            continue;
        }
        int j = to.find(from.at(i).path);
        if (j >= 0) {
            if (!playing.empty() && playing == from.at(i).path) {
                start = (int)kept.size();
            }
            kept.push_back((uint16_t)j);
        }
    }
    bool was = is_shuffled;
    set(kept, start, false, 0);
    if (was) {
        shuffle(true, 1);
    }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

Settings load_settings()
{
    Settings s;
    nvs_handle_t h;
    if (settings_nvs::open(NS, NVS_READONLY, &h) != ESP_OK) {
        return s;
    }
    char folder[ROOT_LEN] = "";
    size_t len = sizeof(folder);
    if (nvs_get_str(h, "dir", folder, &len) == ESP_OK && folder[0] == '/' && strstr(folder, "/..") == nullptr) {
        s.folder = folder;
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, "shuffle", &v) == ESP_OK) {
        s.shuffle = v == 1;
    }
    if (nvs_get_u8(h, "repeat", &v) == ESP_OK && v <= (uint8_t)Repeat::One) {
        s.repeat = (Repeat)v;
    }
    if (nvs_get_u8(h, "online", &v) == ESP_OK) {
        s.online = v != 0;
    }
    if (nvs_get_u8(h, "src", &v) == ESP_OK) {
        s.music_last = v == 1;
    }
    uint32_t last = 0;
    if (nvs_get_u32(h, "last", &last) == ESP_OK) {
        s.last_song = last;
    }
    nvs_close(h);
    return s;
}

esp_err_t save_settings(const Settings& s)
{
    Settings old = load_settings();
    nvs_handle_t h;
    esp_err_t err = settings_nvs::open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    bool dirty = false;
    auto u8 = [&](const char* key, uint8_t now, uint8_t before) {
        if (err == ESP_OK && now != before) {
            err = nvs_set_u8(h, key, now);
            dirty = true;
        }
    };
    if (s.folder != old.folder && s.folder.size() < ROOT_LEN) {
        err = nvs_set_str(h, "dir", s.folder.c_str());
        dirty = true;
    }
    u8("shuffle", s.shuffle, old.shuffle);
    u8("repeat", (uint8_t)s.repeat, (uint8_t)old.repeat);
    u8("online", s.online, old.online);
    u8("src", s.music_last, old.music_last);
    if (err == ESP_OK && s.last_song != old.last_song) {
        err = nvs_set_u32(h, "last", s.last_song);
        dirty = true;
    }
    if (err == ESP_OK && dirty) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

std::vector<uint32_t> load_favorites()
{
    std::vector<uint32_t> out;
    nvs_handle_t h;
    if (settings_nvs::open(NS, NVS_READONLY, &h) != ESP_OK) {
        return out;
    }
    uint32_t buf[MAX_FAVORITES];
    size_t len = sizeof(buf);
    if (nvs_get_blob(h, "fav", buf, &len) == ESP_OK && len % sizeof(uint32_t) == 0) {
        for (size_t i = 0; i < len / sizeof(uint32_t); i++) {
            if (buf[i]) {
                out.push_back(buf[i]);
            }
        }
    }
    nvs_close(h);
    return out;
}

esp_err_t save_favorites(const std::vector<uint32_t>& hashes)
{
    size_t n = std::min(hashes.size(), (size_t)MAX_FAVORITES);
    nvs_handle_t h;
    esp_err_t err = settings_nvs::open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = n ? nvs_set_blob(h, "fav", hashes.data(), n * sizeof(uint32_t)) : nvs_erase_key(h, "fav");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool is_favorite(const std::vector<uint32_t>& hashes, uint32_t hash)
{
    return std::find(hashes.begin(), hashes.end(), hash) != hashes.end();
}

bool toggle_favorite(std::vector<uint32_t>& hashes, uint32_t hash)
{
    auto it = std::find(hashes.begin(), hashes.end(), hash);
    if (it != hashes.end()) {
        hashes.erase(it);
        return true;
    }
    if (hashes.size() >= (size_t)MAX_FAVORITES) {
        return false;
    }
    hashes.push_back(hash);
    return true;
}

}
