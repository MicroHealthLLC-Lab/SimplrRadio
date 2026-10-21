/*
 * Audio file tags (media_tags.c): ID3v1 / ID3v2.2-2.4, FLAC and Ogg comments, MP4 items and
 * WAV INFO, the length of each, names from the path, genres. Files on the SD card are untrusted:
 * truncated, oversized and random files must never read outside the file or crash (ASan).
 */

#include "media_tags.h"
#include "test.hpp"
#include <cstring>
#include <string>

namespace
{
    using Bytes = std::string;

    size_t mem_read(void* ctx, uint64_t off, uint8_t* buf, size_t len)
    {
        const Bytes* b = static_cast<const Bytes*>(ctx);
        if (off >= b->size()) {
            return 0;
        }
        size_t n = std::min(len, (size_t)(b->size() - off));
        memcpy(buf, b->data() + off, n);
        return n;
    }

    bool read(const Bytes& data, media_format_t format, media_tags_t& t)
    {
        media_file_t f = {mem_read, const_cast<Bytes*>(&data), data.size()};
        return media_tags_read(&f, format, &t);
    }

    Bytes be32(uint32_t v) { return Bytes{(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v}; }
    Bytes be16(uint16_t v) { return Bytes{(char)(v >> 8), (char)v}; }
    Bytes le32(uint32_t v) { return Bytes{(char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24)}; }
    Bytes le16(uint16_t v) { return Bytes{(char)v, (char)(v >> 8)}; }
    Bytes le64(uint64_t v) { return le32((uint32_t)v) + le32((uint32_t)(v >> 32)); }
    Bytes syncsafe(uint32_t v)
    {
        return Bytes{(char)((v >> 21) & 0x7F), (char)((v >> 14) & 0x7F), (char)((v >> 7) & 0x7F), (char)(v & 0x7F)};
    }

    Bytes id3_frame(int version, const std::string& id, const Bytes& body)
    {
        if (version == 2) {
            uint32_t n = (uint32_t)body.size();
            return id + Bytes{(char)(n >> 16), (char)(n >> 8), (char)n} + body;
        }
        return id + (version == 4 ? syncsafe((uint32_t)body.size()) : be32((uint32_t)body.size())) + Bytes(2, '\0') + body;
    }

    Bytes id3_tag(int version, const Bytes& frames, size_t padding = 16)
    {
        Bytes all = frames + Bytes(padding, '\0');
        return Bytes("ID3") + (char)version + '\0' + '\0' + syncsafe((uint32_t)all.size()) + all;
    }

    // An MPEG-1 layer III frame header: 128 kbps, 44.1 kHz, stereo
    Bytes mp3_frame(bool xing, uint32_t frames)
    {
        Bytes f = Bytes{(char)0xFF, (char)0xFB, (char)0x90, (char)0x00};
        f += Bytes(32, '\0');   // Side information (MPEG-1 stereo)
        if (xing) {
            f += "Xing" + be32(1) + be32(frames);
        }
        f.resize(417, '\0');
        return f;
    }

    Bytes vorbis_comment(const std::vector<std::string>& entries)
    {
        Bytes b = le32(6) + "vendor" + le32((uint32_t)entries.size());
        for (const auto& e : entries) {
            b += le32((uint32_t)e.size()) + e;
        }
        return b;
    }

    // One Ogg page holding whole packets (each under 255 bytes)
    Bytes ogg_page(uint32_t serial, uint64_t granule, const std::vector<Bytes>& packets, uint8_t type = 0)
    {
        Bytes segs, body;
        for (const auto& p : packets) {
            size_t n = p.size();
            while (n >= 255) {
                segs += (char)255;
                n -= 255;
            }
            segs += (char)n;
            body += p;
        }
        return Bytes("OggS") + '\0' + (char)type + le64(granule) + le32(serial) + le32(0) + le32(0) + (char)segs.size() +
               segs + body;
    }

    Bytes box(const std::string& type, const Bytes& body)
    {
        return be32((uint32_t)(8 + body.size())) + type + body;
    }

    Bytes data_box(uint32_t kind, const Bytes& value)
    {
        return box("data", be32(kind) + be32(0) + value);
    }

    std::string s(const char* text) { return text; }
}

// ---------------------------------------------------------------------------

TEST(media_format_from_name)
{
    CHECK_EQ((int)media_format_from_name("Song.MP3"), (int)MEDIA_MP3);
    CHECK_EQ((int)media_format_from_name("a.b.flac"), (int)MEDIA_FLAC);
    CHECK_EQ((int)media_format_from_name("x.opus"), (int)MEDIA_OPUS);
    CHECK_EQ((int)media_format_from_name("x.m4a"), (int)MEDIA_M4A);
    CHECK_EQ((int)media_format_from_name("x.wav"), (int)MEDIA_WAV);
    CHECK_EQ((int)media_format_from_name(".mp3"), (int)MEDIA_NONE);       // Hidden file
    CHECK_EQ((int)media_format_from_name("cover.jpg"), (int)MEDIA_NONE);
    CHECK_EQ((int)media_format_from_name("README"), (int)MEDIA_NONE);
    CHECK_EQ((int)media_format_from_name(nullptr), (int)MEDIA_NONE);
    CHECK_EQ(s(media_format_name(MEDIA_ALAC)), s("ALAC"));
    CHECK_EQ(s(media_format_name((media_format_t)99)), s(""));
}

TEST(media_id3v23_text_encodings_and_xing_length)
{
    Bytes frames = id3_frame(3, "TIT2", Bytes(1, '\0') + "Caf\xE9 Song") +                       // Latin-1
                   id3_frame(3, "TPE1", Bytes(1, '\1') + "\xFF\xFE" + Bytes("M\0" "\xFC\0" "s\0", 6)) +  // UTF-16 LE
                   id3_frame(3, "TALB", Bytes(1, '\2') + Bytes("\0A\0l\0b", 6)) +                 // UTF-16 BE
                   id3_frame(3, "TCON", Bytes(1, '\0') + "(17)") +
                   id3_frame(3, "TRCK", Bytes(1, '\0') + "3/12") +
                   id3_frame(3, "TYER", Bytes(1, '\0') + "1999") +
                   id3_frame(3, "APIC", Bytes(3000, 'x'));                                         // Skipped
    Bytes tag = id3_tag(3, frames);
    Bytes file = tag + mp3_frame(true, 1000) + Bytes(4000, '\0');
    media_tags_t t;
    CHECK(read(file, MEDIA_MP3, t));
    CHECK_EQ(s(t.title), s("Cafe Song"));
    CHECK_EQ(s(t.artist), s("Mus"));
    CHECK_EQ(s(t.album), s("Alb"));
    CHECK_EQ(s(t.genre), s("Rock"));
    CHECK_EQ((int)t.track, 3);
    CHECK_EQ((int)t.year, 1999);
    CHECK_EQ(t.audio_offset, (uint32_t)tag.size());
    CHECK_EQ(t.duration_ms, (uint32_t)(1000ULL * 1152 * 1000 / 44100));   // Frames from the Xing header
    CHECK(t.playable);
}

TEST(media_id3v24_utf8_syncsafe_and_album_artist)
{
    Bytes frames = id3_frame(4, "TIT2", Bytes(1, '\3') + "Na\xC3\xAFve") + id3_frame(4, "TPE2", Bytes(1, '\3') + "Band") +
                   id3_frame(4, "TDRC", Bytes(1, '\3') + "2013-05-17") +
                   id3_frame(4, "TCON", Bytes(1, '\3') + "Electronic; House");
    Bytes file = id3_tag(4, frames) + mp3_frame(false, 0) + Bytes(16000, '\0');
    media_tags_t t;
    CHECK(read(file, MEDIA_MP3, t));
    CHECK_EQ(s(t.title), s("Naive"));
    CHECK_EQ(s(t.artist), s("Band"));   // The album artist when there is no artist
    CHECK_EQ((int)t.year, 2013);
    CHECK_EQ(s(t.genre), s("Electronic"));
    CHECK(t.duration_ms > 0);           // Constant bitrate estimate
}

TEST(media_id3v22_three_letter_frames)
{
    Bytes frames = id3_frame(2, "TT2", Bytes(1, '\0') + "Old") + id3_frame(2, "TP1", Bytes(1, '\0') + "Tagger");
    media_tags_t t;
    CHECK(read(id3_tag(2, frames) + mp3_frame(false, 0), MEDIA_MP3, t));
    CHECK_EQ(s(t.title), s("Old"));
    CHECK_EQ(s(t.artist), s("Tagger"));
}

TEST(media_id3v1_and_cbr_length)
{
    Bytes v1 = "TAG" + Bytes("Title One") + Bytes(21, '\0') + Bytes("Artist") + Bytes(24, '\0') + Bytes("Album") +
               Bytes(25, '\0') + "2001" + Bytes(28, '\0') + Bytes(1, '\0') + Bytes(1, '\7') + Bytes(1, (char)17);
    CHECK_EQ(v1.size(), (size_t)128);
    Bytes audio = mp3_frame(false, 0) + Bytes(16000 - 417, '\0');   // 16000 bytes at 128 kbps: 1 s
    media_tags_t t;
    CHECK(read(audio + v1, MEDIA_MP3, t));
    CHECK_EQ(s(t.title), s("Title One"));
    CHECK_EQ(s(t.artist), s("Artist"));
    CHECK_EQ(s(t.album), s("Album"));
    CHECK_EQ((int)t.year, 2001);
    CHECK_EQ((int)t.track, 7);
    CHECK_EQ(s(t.genre), s("Rock"));
    CHECK_EQ(t.duration_ms, (uint32_t)1000);
    CHECK_EQ(t.audio_offset, (uint32_t)0);
}

TEST(media_id3v2_tags_win_over_id3v1)
{
    Bytes v1 = "TAG" + Bytes("Old title") + Bytes(21, '\0') + Bytes(94, '\0') + Bytes(1, (char)0);
    Bytes file = id3_tag(3, id3_frame(3, "TIT2", Bytes(1, '\0') + "New title")) + mp3_frame(false, 0) + v1;
    media_tags_t t;
    CHECK(read(file, MEDIA_MP3, t));
    CHECK_EQ(s(t.title), s("New title"));
    CHECK_EQ(s(t.genre), s("Blues"));   // Only ID3v1 had one
}

TEST(media_flac_streaminfo_and_comments)
{
    Bytes info(34, '\0');
    // 44100 Hz (20 bits), 2 channels, 16 bits, 441000 samples (36 bits)
    uint32_t rate = 44100;
    info[10] = (char)(rate >> 12);
    info[11] = (char)(rate >> 4);
    info[12] = (char)(((rate & 0x0F) << 4) | (1 << 1));
    info[13] = (char)(15 << 4);
    Bytes samples = be32(441000);
    info.replace(14, 4, samples);
    Bytes comments = vorbis_comment({"TITLE=Lossless", "artist=FLAC Band", "GENRE=jazz", "TRACKNUMBER=05", "DATE=1960"});
    Bytes file = "fLaC" + Bytes(1, '\0') + Bytes{0, 0, 34} + info + Bytes(1, (char)(0x80 | 4)) +
                 Bytes{(char)(comments.size() >> 16), (char)(comments.size() >> 8), (char)comments.size()} + comments +
                 Bytes(100, '\0');
    media_tags_t t;
    CHECK(read(file, MEDIA_FLAC, t));
    CHECK_EQ(s(t.title), s("Lossless"));
    CHECK_EQ(s(t.artist), s("FLAC Band"));
    CHECK_EQ(s(t.genre), s("Jazz"));
    CHECK_EQ((int)t.track, 5);
    CHECK_EQ((int)t.year, 1960);
    CHECK_EQ(t.duration_ms, (uint32_t)10000);
    CHECK_EQ(t.audio_offset, (uint32_t)0);
}

TEST(media_flac_behind_id3_and_broken_blocks)
{
    Bytes id3 = id3_tag(3, id3_frame(3, "TIT2", Bytes(1, '\0') + "Front"));
    // A comment block that claims more bytes than the file has
    Bytes file = id3 + "fLaC" + Bytes(1, (char)(0x80 | 4)) + Bytes{(char)0x7F, (char)0xFF, (char)0xFF} + "short";
    media_tags_t t;
    CHECK(read(file, MEDIA_FLAC, t));
    CHECK_EQ(s(t.title), s("Front"));
    CHECK_EQ(t.audio_offset, (uint32_t)id3.size());   // The decoder starts at "fLaC"
    CHECK(!read(Bytes("RIFFnot flac at all"), MEDIA_FLAC, t));
}

TEST(media_ogg_vorbis_comments_and_length)
{
    Bytes ident = Bytes("\x01vorbis", 7) + le32(0) + Bytes(1, '\2') + le32(48000) + Bytes(16, '\0');
    Bytes comment = Bytes("\x03vorbis", 7) + vorbis_comment({"TITLE=Ogg Song", "ARTIST=Ogg Band", "ALBUM=Ogg Album"}) + "\x01";
    Bytes file = ogg_page(7, 0, {ident}, 2) + ogg_page(7, 0, {comment}) + Bytes(5000, 'a') +
                 ogg_page(9, 999999999, {Bytes("other stream")}) + ogg_page(7, 48000 * 3, {Bytes("audio")}, 4);
    media_tags_t t;
    CHECK(read(file, MEDIA_VORBIS, t));
    CHECK_EQ((int)t.format, (int)MEDIA_VORBIS);
    CHECK_EQ(s(t.title), s("Ogg Song"));
    CHECK_EQ(s(t.artist), s("Ogg Band"));
    CHECK_EQ(s(t.album), s("Ogg Album"));
    CHECK_EQ(t.duration_ms, (uint32_t)3000);   // The last page of this stream, not the other one
}

TEST(media_opus_in_ogg)
{
    Bytes head = Bytes("OpusHead") + Bytes(1, '\1') + Bytes(1, '\2') + le16(312) + le32(48000) + Bytes(3, '\0');
    Bytes tags = Bytes("OpusTags") + vorbis_comment({"title=Opus Song", "ARTIST=opus band"});
    Bytes file = ogg_page(1, 0, {head}, 2) + ogg_page(1, 0, {tags}) + ogg_page(1, 312 + 48000 * 2, {Bytes("x")}, 4);
    media_tags_t t;
    CHECK(read(file, MEDIA_VORBIS, t));   // Named .ogg, holds Opus
    CHECK_EQ((int)t.format, (int)MEDIA_OPUS);
    CHECK_EQ(s(t.title), s("Opus Song"));
    CHECK_EQ(s(t.artist), s("opus band"));   // Fixed only for the library's names (media_tags_from_path)
    CHECK_EQ(t.duration_ms, (uint32_t)2000);
    CHECK(!read(ogg_page(1, 0, {Bytes("fLaC-in-ogg")}), MEDIA_VORBIS, t));
}

TEST(media_mp4_items_alac_and_moov_order)
{
    Bytes ilst = box("ilst", box("\xA9nam", data_box(1, "M4A Title")) + box("\xA9" "ART", data_box(1, "M4A Artist")) +
                                 box("\xA9" "alb", data_box(1, "M4A Album")) + box("gnre", data_box(0, be16(9))) +
                                 box("trkn", data_box(0, be16(0) + be16(4) + be16(10))) +
                                 box("\xA9" "day", data_box(1, "2005-01-01")));
    Bytes meta = box("meta", be32(0) + box("hdlr", Bytes(25, '\0')) + ilst);
    Bytes mvhd = box("mvhd", Bytes(1, '\0') + Bytes(11, '\0') + be32(1000) + be32(5000) + Bytes(80, '\0'));
    Bytes stsd = box("stsd", be32(0) + be32(1) + box("alac", Bytes(28, '\0')));
    Bytes trak = box("trak", box("mdia", box("minf", box("stbl", stsd))));
    Bytes moov = box("moov", mvhd + trak + box("udta", meta));
    Bytes ftyp = box("ftyp", Bytes("M4A ") + be32(0));
    Bytes mdat = box("mdat", Bytes(64, 'z'));
    media_tags_t t;
    CHECK(read(ftyp + moov + mdat, MEDIA_M4A, t));
    CHECK_EQ(s(t.title), s("M4A Title"));
    CHECK_EQ(s(t.artist), s("M4A Artist"));
    CHECK_EQ(s(t.album), s("M4A Album"));
    CHECK_EQ(s(t.genre), s("Jazz"));    // gnre is the ID3 genre + 1: 9 is genre 8
    CHECK_EQ((int)t.track, 4);
    CHECK_EQ((int)t.year, 2005);
    CHECK_EQ(t.duration_ms, (uint32_t)5000);
    CHECK_EQ((int)t.format, (int)MEDIA_ALAC);
    CHECK(t.playable);
    // The index after the audio: the decoder can't stream it
    CHECK(read(ftyp + mdat + moov, MEDIA_M4A, t));
    CHECK(!t.playable);
    CHECK(!read(Bytes("not an mp4 file"), MEDIA_M4A, t));
}

TEST(media_wav_info_and_length)
{
    Bytes fmt = le16(1) + le16(2) + le32(44100) + le32(176400) + le16(4) + le16(16);
    Bytes info = Bytes("INFO") + "INAM" + le32(9) + "Wav Title" + Bytes(1, '\0') + "IART" + le32(4) + "Band";
    Bytes body = Bytes("WAVE") + "fmt " + le32(16) + fmt + "LIST" + le32((uint32_t)info.size()) + info + "data" +
                 le32(176400 * 2) + Bytes(176400 * 2, '\0');
    Bytes file = "RIFF" + le32((uint32_t)body.size()) + body;
    media_tags_t t;
    CHECK(read(file, MEDIA_WAV, t));
    CHECK_EQ(s(t.title), s("Wav Title"));
    CHECK_EQ(s(t.artist), s("Band"));
    CHECK_EQ(t.duration_ms, (uint32_t)2000);
    CHECK(t.playable);
    Bytes flt = le16(3) + le16(2) + le32(44100) + le32(352800) + le16(8) + le16(32);
    Bytes fbody = Bytes("WAVE") + "fmt " + le32(16) + flt + "data" + le32(8) + Bytes(8, '\0');
    CHECK(read("RIFF" + le32((uint32_t)fbody.size()) + fbody, MEDIA_WAV, t));
    CHECK(!t.playable);   // 32-bit float
}

TEST(media_amr_magic)
{
    media_tags_t t;
    CHECK(read(Bytes("#!AMR\n") + Bytes(40, '\0'), MEDIA_AMR, t));
    CHECK(read(Bytes("#!AMR-WB\n") + Bytes(40, '\0'), MEDIA_AMRWB, t));
    CHECK(!read(Bytes("#!AMR\n") + Bytes(40, '\0'), MEDIA_AMRWB, t));
}

// ---------------------------------------------------------------------------
// Names from the path, capitals, genres
// ---------------------------------------------------------------------------

static media_tags_t from_path(const char* path)
{
    media_tags_t t = {};
    media_tags_from_path(path, &t);
    return t;
}

TEST(media_names_from_folders_and_file)
{
    media_tags_t t = from_path("Daft Punk/Discovery/03 - Digital Love.mp3");
    CHECK_EQ(s(t.artist), s("Daft Punk"));
    CHECK_EQ(s(t.album), s("Discovery"));
    CHECK_EQ(s(t.title), s("Digital Love"));
    CHECK_EQ((int)t.track, 3);

    t = from_path("Massive Attack - Mezzanine/01 Angel.ogg");
    CHECK_EQ(s(t.artist), s("Massive Attack"));
    CHECK_EQ(s(t.album), s("Mezzanine"));
    CHECK_EQ(s(t.title), s("Angel"));
    CHECK_EQ((int)t.track, 1);

    t = from_path("Portishead - Roads.flac");
    CHECK_EQ(s(t.artist), s("Portishead"));
    CHECK_EQ(s(t.title), s("Roads"));

    t = from_path("Abbey Road/CD2/1-04 Something.mp3");
    CHECK_EQ(s(t.album), s("Abbey Road"));   // "CD2" is skipped
    CHECK_EQ(s(t.title), s("Something"));
    CHECK_EQ((int)t.track, 4);

    t = from_path("Mix/07_the_track_name.mp3");
    CHECK_EQ(s(t.title), s("The Track Name"));   // Underscores, all lower case fixed
    CHECK_EQ((int)t.track, 7);
}

TEST(media_names_keep_numbers_that_are_titles)
{
    CHECK_EQ(s(from_path("99 Luftballons.mp3").title), s("99 Luftballons"));
    CHECK_EQ(s(from_path("1979.mp3").title), s("1979"));
    CHECK_EQ(s(from_path("24.mp3").title), s("24"));
}

TEST(media_names_never_replace_tags)
{
    media_tags_t t = {};
    snprintf(t.title, sizeof(t.title), "%s", "Tagged");
    snprintf(t.artist, sizeof(t.artist), "%s", "Tag Artist");
    media_tags_from_path("Other Artist/Other Album/01 - Other.mp3", &t);
    CHECK_EQ(s(t.title), s("Tagged"));
    CHECK_EQ(s(t.artist), s("Tag Artist"));
    CHECK_EQ(s(t.album), s("Other Album"));   // Only the empty field is filled
}

TEST(media_fix_case_only_when_clearly_wrong)
{
    auto fixed = [](const char* in) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", in);
        media_fix_case(buf);
        return std::string(buf);
    };
    CHECK_EQ(fixed("daft punk"), s("Daft Punk"));
    CHECK_EQ(fixed("LED ZEPPELIN"), s("Led Zeppelin"));
    CHECK_EQ(fixed("ABBA"), s("ABBA"));
    CHECK_EQ(fixed("AC/DC"), s("AC/DC"));
    CHECK_EQ(fixed("R.E.M."), s("R.E.M."));
    CHECK_EQ(fixed("iPod Song"), s("iPod Song"));
    CHECK_EQ(fixed("deadmau5"), s("Deadmau5"));
}

TEST(media_genre_clean_variants)
{
    auto g = [](const char* in) {
        char buf[MEDIA_GENRE_LEN];
        media_genre_clean(buf, sizeof(buf), in);
        return std::string(buf);
    };
    CHECK_EQ(g("(17)"), s("Rock"));
    CHECK_EQ(g("17"), s("Rock"));
    CHECK_EQ(g("(17)Indie"), s("Indie"));
    CHECK_EQ(g("(RX)"), s("Remix"));
    CHECK_EQ(g("rock; pop"), s("Rock"));
    CHECK_EQ(g("hip hop/rap"), s("Hip-Hop"));
    CHECK_EQ(g("r&b"), s("R&B"));
    CHECK_EQ(g("electronica"), s("Electronic"));
    CHECK_EQ(g("indie pop"), s("Indie Pop"));
    CHECK_EQ(g(""), s(""));
    CHECK_EQ(g("Unknown"), s(""));
    CHECK_EQ(g("999"), s(""));
    CHECK_EQ(g(nullptr), s(""));
    CHECK(media_id3_genre(191) != nullptr);
    CHECK(media_id3_genre(192) == nullptr);
    CHECK(media_id3_genre(-1) == nullptr);
}

// ---------------------------------------------------------------------------
// Untrusted files
// ---------------------------------------------------------------------------

TEST(media_every_truncation_is_safe)
{
    Bytes id3 = id3_tag(4, id3_frame(4, "TIT2", Bytes(1, '\3') + "Cut") + id3_frame(4, "TPE1", Bytes(1, '\1') + "\xFF\xFE" "A"));
    Bytes flac = Bytes("fLaC") + Bytes(1, (char)(0x80 | 4)) + Bytes{0, 0, 40} + vorbis_comment({"TITLE=x", "ARTIST=y"});
    Bytes ogg = ogg_page(1, 0, {Bytes("OpusHead") + Bytes(11, '\1')}, 2) + ogg_page(1, 0, {Bytes("OpusTags") + vorbis_comment({"A=b"})});
    Bytes mp4 = box("ftyp", Bytes(8, 'm')) + box("moov", box("udta", box("meta", be32(0) + box("ilst", box("\xA9nam", data_box(1, "T"))))));
    Bytes wav = Bytes("RIFF") + le32(100) + "WAVEfmt " + le32(16) + Bytes(16, '\1') + "LIST" + le32(20) + "INFOINAM" + le32(4) + "abcd";
    struct Sample {
        const Bytes* data;
        media_format_t format;
    } samples[] = {{&id3, MEDIA_MP3}, {&flac, MEDIA_FLAC}, {&ogg, MEDIA_OPUS}, {&mp4, MEDIA_M4A}, {&wav, MEDIA_WAV}};
    for (const auto& sample : samples) {
        for (size_t n = 0; n <= sample.data->size(); n++) {
            media_tags_t t;
            read(sample.data->substr(0, n), sample.format, t);   // Must not read past n (ASan)
            CHECK(strlen(t.title) < sizeof(t.title));
        }
    }
}

TEST(media_random_bytes_are_safe)
{
    uint32_t state = 12345;
    auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    const Bytes heads[] = {Bytes("ID3\x03", 4), Bytes("ID3\x04", 4), Bytes("fLaC"), Bytes("OggS"), Bytes("RIFF"),
                           Bytes("\0\0\0\x20" "ftyp", 8), Bytes("#!AMR\n"), Bytes("TAG")};
    for (int i = 0; i < 3000; i++) {
        Bytes data(next() % 1500, '\0');
        for (char& c : data) {
            c = (char)next();
        }
        const Bytes& head = heads[i % (sizeof(heads) / sizeof(heads[0]))];
        size_t n = std::min(data.size(), head.size());
        data.replace(0, n, head, 0, n);
        for (int f = MEDIA_MP3; f < MEDIA_FORMATS; f++) {
            media_tags_t t;
            read(data, (media_format_t)f, t);
            CHECK(strlen(t.title) < sizeof(t.title) && strlen(t.genre) < sizeof(t.genre));
            for (const char* p = t.title; *p; p++) {
                CHECK(*p >= 0x20 && *p < 0x7F);   // Printable ASCII only
            }
        }
    }
}
