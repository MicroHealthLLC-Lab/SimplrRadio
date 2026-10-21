/*
 * Music library
 * The songs found in the music folder on the SD card, as fixed-size records (big arrays live in
 * PSRAM on the radio: big_alloc()), and everything the Music menus need from them: all songs,
 * the artists / albums / genres with their songs, search, favorites, the play queue (shuffle,
 * repeat), the index file kept on the card so the folder isn't read again at every start, and
 * the genres looked up online per artist. The music settings (NVS namespace "music"): the
 * folder, favorites, shuffle, repeat, online lookups, what played last.
 * No ESP-IDF or LVGL headers apart from NVS through settings_nvs, so it is tested on the host
 * (test/host). The index file is untrusted input (anyone can write the card): it is checked
 * (magic, version, sizes, CRC, the folder it was made for) and its text is made printable.
 */

#ifndef MUSIC_LIBRARY_HPP
#define MUSIC_LIBRARY_HPP

#include "esp_err.h"
#include "media_tags.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace music
{
    static const int MAX_TRACKS = 3000;          // Songs in the library
    static const size_t PATH_LEN = 192;          // A song's path under the music folder
    static const size_t ROOT_LEN = 128;          // The music folder's path
    static const int MAX_FAVORITES = 256;
    static const int MAX_GENRE_CACHE = 1000;     // Artists whose genre was looked up online
    static const int MAX_FOLDER_DEPTH = 8;       // Folders below the music folder that are read

    // Arrays of many entries: in PSRAM on the radio (internal RAM keeps small allocations)
    void* big_alloc(size_t bytes);
    void big_free(void* p);

    template <typename T> struct BigAllocator {
        using value_type = T;
        BigAllocator() = default;
        template <typename U> BigAllocator(const BigAllocator<U>&) {}
        T* allocate(size_t n)
        {
            T* p = static_cast<T*>(big_alloc(n * sizeof(T)));
            if (!p) {
                abort();   // As std::allocator does without exceptions
            }
            return p;
        }
        void deallocate(T* p, size_t) { big_free(p); }
        template <typename U> bool operator==(const BigAllocator<U>&) const { return true; }
        template <typename U> bool operator!=(const BigAllocator<U>&) const { return false; }
    };
    using Indexes = std::vector<uint16_t, BigAllocator<uint16_t>>;

    enum Flag : uint8_t {
        GENRE_ONLINE = 1,     // The genre came from the online lookup, not the file
        UNPLAYABLE = 2,       // A variant the decoder can't play (shown, skipped when playing)
    };

    struct Track {
        char path[PATH_LEN];                 // Under the music folder, as the card names it
        char title[MEDIA_TITLE_LEN];         // Printable ASCII (media_tags.h)
        char artist[MEDIA_NAME_LEN];
        char album[MEDIA_NAME_LEN];
        char genre[MEDIA_GENRE_LEN];
        uint32_t duration_ms;
        uint32_t audio_offset;
        uint16_t year;
        uint16_t track;
        uint8_t format;                      // media_format_t
        uint8_t flags;                       // Flag
        uint16_t reserved;
    };

    enum class Group : uint8_t { Artist, Album, Genre };

    struct GroupEntry {
        uint16_t first;                      // A song of the group (its name comes from it)
        uint16_t count;
    };
    using Groups = std::vector<GroupEntry, BigAllocator<GroupEntry>>;

    // "Unknown artist" etc. for a song without one
    std::string group_name(const Track& t, Group g);

    // A song's path hash: favorites and the last song survive a new scan (FNV-1a)
    uint32_t path_hash(const char* path);

    class Library
    {
    public:
        Library() = default;
        ~Library();
        Library(const Library&) = delete;
        Library& operator=(const Library&) = delete;

        bool reserve();                      // Room for MAX_TRACKS; false without memory
        void clear() { count = 0; }
        void swap(Library& other);
        int size() const { return count; }
        const Track& at(int i) const { return tracks[i]; }
        Track& at(int i) { return tracks[i]; }
        Track* add();                        // nullptr when full (or not reserved)

        // Every song: by artist, album, track number, title
        Indexes all() const;
        // The songs of one group: an album in track order; an artist and a genre by album
        Indexes in_group(Group g, int any_song_of_it) const;
        // The groups, sorted by name (unknowns last), each with a song and its number of songs.
        // Albums are told apart by their folder too ("Greatest Hits" of two artists).
        Groups groups(Group g) const;
        // Songs whose title, artist or album contain every word (case-insensitive)
        Indexes search(const std::string& words) const;
        // Songs whose path hash is in `hashes`, in all() order
        Indexes with_hashes(const std::vector<uint32_t>& hashes) const;
        int find(const char* path) const;    // -1 if none
        int find_hash(uint32_t hash) const;

        // Distinct artists of songs without a genre (for the online lookup), at most `max`,
        // leaving out those `skip` says were already asked about
        std::vector<std::string> artists_without_genre(size_t max, const std::function<bool(const char*)>& skip) const;
        // The first song from `from` on without a genre whose artist `skip` doesn't rule out, -1
        // if none: the online lookup walks the library with it, a step at a time
        int next_without_genre(int from, const std::function<bool(const char*)>& skip) const;
        // Gives every song of `artist` without a genre this one; how many songs changed
        int set_artist_genre(const std::string& artist, const std::string& genre);
        // Fixes the capitals of an artist's name everywhere ("daft punk" -> "Daft Punk")
        int set_artist_name(const std::string& from, const std::string& to);

        // The index file: write / read through callbacks (the SD card, or memory in tests)
        bool save(const std::string& root, const std::function<bool(const void*, size_t)>& write) const;
        bool load(const std::string& root, const std::function<size_t(void*, size_t)>& read);

    private:
        Track* tracks = nullptr;
        int count = 0;
        bool same_group(Group g, const Track& a, const Track& b) const;
        int compare(Group g, const Track& a, const Track& b) const;
    };

    // Genres found online per artist (and artists found without one, so they aren't asked again)
    class GenreCache
    {
    public:
        struct Entry {
            char artist[MEDIA_NAME_LEN];     // As the library spells it
            char genre[MEDIA_GENRE_LEN];     // "" = looked up, none found
        };
        bool known(const std::string& artist) const;
        std::string genre(const std::string& artist) const;
        void put(const std::string& artist, const std::string& genre);
        size_t size() const { return entries.size(); }
        const std::vector<Entry, BigAllocator<Entry>>& all() const { return entries; }
        bool save(const std::function<bool(const void*, size_t)>& write) const;
        bool load(const std::function<size_t(void*, size_t)>& read);
    private:
        std::vector<Entry, BigAllocator<Entry>> entries;
        int index_of(const std::string& artist) const;
    };

    enum class Repeat : uint8_t { Off, All, One };

    // What plays: a list of songs and where in it, in order or shuffled
    class Queue
    {
    public:
        void set(const Indexes& songs, int start, bool shuffle, uint32_t seed);
        void clear();
        int size() const { return (int)items.size(); }
        bool empty() const { return items.empty(); }
        int current() const;                 // The song (library index), -1 if none
        int position() const { return pos; } // In play order
        const Indexes& songs() const { return items; }   // In list order
        int list_position() const;           // The current song's place in songs()
        // The next / previous song (wrapping with Repeat::All); false at an end with Repeat::Off.
        // Repeat::One only holds for a song that ended by itself (`ended`).
        bool step(int delta, Repeat repeat, bool ended);
        void jump(int list_index);           // Play songs()[list_index] next
        void shuffle(bool on, uint32_t seed);
        bool shuffled() const { return is_shuffled; }
        // After a new scan: the same songs by path, in the new library
        void remap(const Library& from, const Library& to);
    private:
        Indexes items;                       // Library indexes, list order
        Indexes order;                       // Positions in items, play order
        int pos = -1;
        bool is_shuffled = false;
    };

    // NVS namespace "music"
    struct Settings {
        std::string folder;                  // "" = not chosen yet (the card's root is offered)
        bool shuffle = false;
        Repeat repeat = Repeat::All;
        bool online = true;                  // Look up missing genres on musicbrainz.org
        bool music_last = false;             // Music (not the radio) played last
        uint32_t last_song = 0;              // Its path hash
    };
    Settings load_settings();
    esp_err_t save_settings(const Settings& s);   // Writes only what changed

    std::vector<uint32_t> load_favorites();
    esp_err_t save_favorites(const std::vector<uint32_t>& hashes);   // At most MAX_FAVORITES
    // Adds or removes a song; false when the list is full
    bool toggle_favorite(std::vector<uint32_t>& hashes, uint32_t hash);
    bool is_favorite(const std::vector<uint32_t>& hashes, uint32_t hash);

    // "3:07", "1:02:03", "" for 0
    std::string duration_text(uint32_t ms);
    // CRC-32 (IEEE) of the index files
    uint32_t crc32(uint32_t crc, const void* data, size_t len);
}

#endif
