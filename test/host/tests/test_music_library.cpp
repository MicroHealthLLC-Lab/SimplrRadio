/*
 * Music library (music_library.cpp): order, artists / albums / genres, search, favorites, the
 * play queue (shuffle, repeat), and the index files on the card, which are untrusted: a file
 * that was changed, cut short or made for another folder is refused, and bad records dropped.
 */

#include "music_library.hpp"
#include "test.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <strings.h>

using namespace music;

namespace
{
    void add(Library& lib, const char* path, const char* title, const char* artist, const char* album,
             const char* genre = "", int track = 0)
    {
        Track* t = lib.add();
        CHECK(t != nullptr);
        if (!t) {
            return;
        }
        snprintf(t->path, sizeof(t->path), "%s", path);
        snprintf(t->title, sizeof(t->title), "%s", title);
        snprintf(t->artist, sizeof(t->artist), "%s", artist);
        snprintf(t->album, sizeof(t->album), "%s", album);
        snprintf(t->genre, sizeof(t->genre), "%s", genre);
        t->track = (uint16_t)track;
        t->format = MEDIA_MP3;
    }

    // A small library: two artists, an unknown one, two "Greatest Hits" in different folders
    void sample(Library& lib)
    {
        CHECK(lib.reserve());
        add(lib, "Zed/Live/02.mp3", "Second", "Zed", "Live", "Rock", 2);
        add(lib, "Zed/Live/01.mp3", "First", "zed", "Live", "rock", 1);
        add(lib, "abba/Greatest Hits/01.mp3", "Waterloo", "ABBA", "Greatest Hits", "Pop", 1);
        add(lib, "Queen/Greatest Hits/01.mp3", "Bohemian Rhapsody", "Queen", "Greatest Hits", "Rock", 1);
        add(lib, "loose.mp3", "Loose", "", "", "");
    }

    std::string titles(const Library& lib, const Indexes& list)
    {
        std::string out;
        for (uint16_t i : list) {
            out += (out.empty() ? "" : ",") + std::string(lib.at(i).title);
        }
        return out;
    }

    struct Memory {
        std::string data;
        size_t pos = 0;
        std::function<bool(const void*, size_t)> writer()
        {
            return [this](const void* p, size_t n) {
                data.append(static_cast<const char*>(p), n);
                return true;
            };
        }
        std::function<size_t(void*, size_t)> reader()
        {
            return [this](void* p, size_t n) {
                size_t k = std::min(n, data.size() - pos);
                memcpy(p, data.data() + pos, k);
                pos += k;
                return k;
            };
        }
    };
}

TEST(music_all_songs_in_order)
{
    Library lib;
    sample(lib);
    // By artist (case-insensitive), album, track; the song without an artist last
    CHECK_EQ(titles(lib, lib.all()), std::string("Waterloo,Bohemian Rhapsody,First,Second,Loose"));
}

TEST(music_groups_and_their_songs)
{
    Library lib;
    sample(lib);
    Groups artists = lib.groups(Group::Artist);
    CHECK_EQ(artists.size(), (size_t)4);   // ABBA, Queen, Zed (both spellings), unknown
    CHECK_EQ(group_name(lib.at(artists[2].first), Group::Artist), std::string("Zed"));
    CHECK_EQ((int)artists[2].count, 2);
    CHECK_EQ(group_name(lib.at(artists[3].first), Group::Artist), std::string("Unknown artist"));

    Groups albums = lib.groups(Group::Album);
    CHECK_EQ(albums.size(), (size_t)4);   // Two "Greatest Hits" (other folders), Live, unknown
    Groups genres = lib.groups(Group::Genre);
    CHECK_EQ(genres.size(), (size_t)3);   // Pop, Rock (both spellings), unknown

    int live = lib.find("Zed/Live/02.mp3");
    CHECK_EQ(titles(lib, lib.in_group(Group::Album, live)), std::string("First,Second"));   // Track order
    int queen = lib.find("Queen/Greatest Hits/01.mp3");
    CHECK_EQ(titles(lib, lib.in_group(Group::Album, queen)), std::string("Bohemian Rhapsody"));
    CHECK_EQ(titles(lib, lib.in_group(Group::Genre, queen)), std::string("Bohemian Rhapsody,First,Second"));
    CHECK(lib.in_group(Group::Artist, -1).empty());
}

TEST(music_search_every_word)
{
    Library lib;
    sample(lib);
    CHECK_EQ(titles(lib, lib.search("greatest QUEEN")), std::string("Bohemian Rhapsody"));
    CHECK_EQ(titles(lib, lib.search("live")), std::string("First,Second"));
    CHECK(lib.search("nothing here").empty());
    CHECK(lib.search("   ").empty());
}

TEST(music_favorites_by_path)
{
    Library lib;
    sample(lib);
    std::vector<uint32_t> favs;
    CHECK(toggle_favorite(favs, path_hash("Queen/Greatest Hits/01.mp3")));
    CHECK(toggle_favorite(favs, path_hash("loose.mp3")));
    CHECK_EQ(titles(lib, lib.with_hashes(favs)), std::string("Bohemian Rhapsody,Loose"));
    CHECK(toggle_favorite(favs, path_hash("loose.mp3")));   // Removed again
    CHECK_EQ(favs.size(), (size_t)1);
    std::vector<uint32_t> full;
    for (int i = 0; i < MAX_FAVORITES; i++) {
        CHECK(toggle_favorite(full, (uint32_t)i + 1));
    }
    CHECK(!toggle_favorite(full, 999999));   // Bounded
    CHECK(path_hash("") != 0);
}

TEST(music_online_genres_per_artist)
{
    Library lib;
    sample(lib);
    snprintf(lib.at(lib.find("Zed/Live/01.mp3")).genre, MEDIA_GENRE_LEN, "%s", "");
    snprintf(lib.at(lib.find("Zed/Live/02.mp3")).genre, MEDIA_GENRE_LEN, "%s", "");
    std::vector<std::string> todo = lib.artists_without_genre(10, nullptr);
    CHECK_EQ(todo.size(), (size_t)1);   // One "Zed" whatever its capitals; no unknown artist
    CHECK(lib.artists_without_genre(10, [](const char* a) { return strcmp(a, "zed") == 0 || strcmp(a, "Zed") == 0; })
              .empty());
    int first = lib.next_without_genre(0, nullptr);
    CHECK(first >= 0 && strcasecmp(lib.at(first).artist, "zed") == 0);
    CHECK_EQ(lib.next_without_genre(first + 1, [](const char*) { return true; }), -1);
    CHECK_EQ(lib.next_without_genre(lib.size(), nullptr), -1);
    CHECK_EQ(lib.set_artist_genre("ZED", "Post-Rock"), 2);
    CHECK_EQ(lib.next_without_genre(0, nullptr), -1);   // The song without an artist isn't asked about
    CHECK(lib.at(lib.find("Zed/Live/01.mp3")).flags & GENRE_ONLINE);
    CHECK_EQ(lib.set_artist_name("zed", "ZED"), 2);
    CHECK_EQ(lib.set_artist_name("ZED", "Another Band"), 0);   // Only capitals may change
}

TEST(music_index_round_trip)
{
    Library lib;
    sample(lib);
    Memory m;
    CHECK(lib.save("/Music", m.writer()));
    Library back;
    CHECK(back.reserve());
    CHECK(back.load("/Music", m.reader()));
    CHECK_EQ(back.size(), lib.size());
    CHECK_EQ(titles(back, back.all()), titles(lib, lib.all()));
}

TEST(music_index_refused_when_wrong)
{
    Library lib;
    sample(lib);
    Memory m;
    CHECK(lib.save("/Music", m.writer()));
    Library back;
    CHECK(back.reserve());
    Memory other = m;
    CHECK(!back.load("/Other", other.reader()));   // Made for another folder
    CHECK_EQ(back.size(), 0);
    Memory changed = m;
    changed.data[200] ^= 0x01;                     // A changed byte: CRC
    CHECK(!back.load("/Music", changed.reader()));
    Memory cut = m;
    cut.data.resize(cut.data.size() - 10);         // Cut short
    CHECK(!back.load("/Music", cut.reader()));
    Memory empty;
    CHECK(!back.load("/Music", empty.reader()));
    Memory junk;
    junk.data = std::string(5000, 'x');
    CHECK(!back.load("/Music", junk.reader()));
    Library unreserved;
    Memory again = m;
    CHECK(!unreserved.load("/Music", again.reader()));
}

TEST(music_index_bad_records_dropped)
{
    Library lib;
    CHECK(lib.reserve());
    add(lib, "../outside.mp3", "Escape", "x", "y");
    add(lib, "/absolute.mp3", "Absolute", "x", "y");
    add(lib, "ok/./dot.mp3", "Dot", "x", "y");
    add(lib, "fine/song.mp3", "Fine\x01\x7F", "x", "y");
    Track* bad_format = lib.add();
    snprintf(bad_format->path, sizeof(bad_format->path), "%s", "bad.mp3");
    bad_format->format = 200;
    Memory m;
    CHECK(lib.save("/", m.writer()));
    Library back;
    CHECK(back.reserve());
    CHECK(back.load("/", m.reader()));
    CHECK_EQ(back.size(), 1);
    CHECK_EQ(std::string(back.at(0).title), std::string("Fine??"));   // Made printable
}

TEST(music_library_bounded)
{
    Library lib;
    CHECK(lib.reserve());
    for (int i = 0; i < MAX_TRACKS; i++) {
        CHECK(lib.add() != nullptr);
    }
    CHECK(lib.add() == nullptr);
    Library none;
    CHECK(none.add() == nullptr);   // Not reserved
}

TEST(music_genre_cache_round_trip_and_bound)
{
    GenreCache cache;
    cache.put("Daft Punk", "Electronic");
    cache.put("Nobody Knows", "");
    CHECK(cache.known("daft punk"));
    CHECK_EQ(cache.genre("DAFT PUNK"), std::string("Electronic"));
    CHECK(cache.known("Nobody Knows"));
    CHECK_EQ(cache.genre("Nobody Knows"), std::string(""));
    CHECK(!cache.known("Someone Else"));
    Memory m;
    CHECK(cache.save(m.writer()));
    GenreCache back;
    CHECK(back.load(m.reader()));
    CHECK_EQ(back.size(), (size_t)2);
    CHECK_EQ(back.genre("daft punk"), std::string("Electronic"));
    m.data[m.data.size() - 1] ^= 0x10;
    m.pos = 0;
    CHECK(!back.load(m.reader()));
    GenreCache big;
    for (int i = 0; i < MAX_GENRE_CACHE + 5; i++) {
        big.put("Artist " + std::to_string(i), "Rock");
    }
    CHECK_EQ(big.size(), (size_t)MAX_GENRE_CACHE);
    CHECK(!big.known("Artist 0"));   // The oldest went
}

TEST(music_queue_order_and_repeat)
{
    Indexes songs;
    for (uint16_t i = 0; i < 4; i++) {
        songs.push_back((uint16_t)(10 + i));
    }
    Queue q;
    q.set(songs, 2, false, 0);
    CHECK_EQ(q.current(), 12);
    CHECK(q.step(1, Repeat::Off, true));
    CHECK_EQ(q.current(), 13);
    CHECK(!q.step(1, Repeat::Off, true));     // The end of the list, repeat off: stop
    CHECK(q.step(1, Repeat::Off, false));     // Next by hand wraps
    CHECK_EQ(q.current(), 10);
    CHECK(q.step(-1, Repeat::All, false));
    CHECK_EQ(q.current(), 13);
    CHECK(q.step(1, Repeat::All, true));      // Repeat the list
    CHECK_EQ(q.current(), 10);
    CHECK(q.step(1, Repeat::One, true));      // Repeat the song
    CHECK_EQ(q.current(), 10);
    q.jump(3);
    CHECK_EQ(q.current(), 13);
    CHECK_EQ(q.list_position(), 3);
    Queue empty;
    CHECK(!empty.step(1, Repeat::All, false));
    CHECK_EQ(empty.current(), -1);
}

TEST(music_queue_shuffle_is_a_permutation_starting_with_the_pick)
{
    Indexes songs;
    for (uint16_t i = 0; i < 50; i++) {
        songs.push_back(i);
    }
    Queue q;
    q.set(songs, 17, true, 1234);
    CHECK(q.shuffled());
    CHECK_EQ(q.current(), 17);                // The song picked plays first
    std::vector<int> seen;
    seen.push_back(q.current());
    for (int i = 1; i < 50; i++) {
        CHECK(q.step(1, Repeat::Off, true));
        seen.push_back(q.current());
    }
    std::sort(seen.begin(), seen.end());
    for (int i = 0; i < 50; i++) {
        CHECK_EQ(seen[i], i);                 // Every song once
    }
    q.shuffle(false, 0);
    CHECK(!q.shuffled());
}

TEST(music_queue_follows_a_new_scan)
{
    Library old_lib, new_lib;
    sample(old_lib);
    CHECK(new_lib.reserve());
    add(new_lib, "new.mp3", "New", "N", "N");
    add(new_lib, "Zed/Live/01.mp3", "First", "zed", "Live");
    add(new_lib, "Zed/Live/02.mp3", "Second", "Zed", "Live");
    Queue q;
    q.set(old_lib.all(), 3, false, 0);   // "Second"
    CHECK_EQ(std::string(old_lib.at(q.current()).title), std::string("Second"));
    q.remap(old_lib, new_lib);
    CHECK_EQ(q.size(), 2);                // Songs no longer on the card are gone
    CHECK_EQ(std::string(new_lib.at(q.current()).title), std::string("Second"));
}

TEST(music_duration_text)
{
    CHECK_EQ(duration_text(0), std::string(""));
    CHECK_EQ(duration_text(187000), std::string("3:07"));
    CHECK_EQ(duration_text(3723000), std::string("1:02:03"));
}

TEST(music_crc32_known_value)
{
    CHECK_EQ(crc32(0, "123456789", 9), (uint32_t)0xCBF43926);
}
