// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The rekordbox baseline file of Sync after Rekordbox Export
// (src/infrastructure/local/rekordbox_baseline_file.hpp). The baseline is
// the B of a three-way merge: a field that does not survive the file is a
// wrong proposal on the next run, and a damaged file read as "no baseline"
// would turn the next run into the first-use fallback. So every field is
// compared after a round trip, and every way the file can be wrong must
// come back as an error that names it.

#include "infrastructure/local/rekordbox_baseline_file.hpp"

#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using seabass::pathFromUtf8;
using seabass::pathToUtf8;
using seabass::domain::BaselineCue;
using seabass::domain::BaselinePlaylist;
using seabass::domain::BaselineTrack;
using seabass::domain::CuePoint;
using seabass::domain::RekordboxBaseline;
using seabass::domain::ValueOrigin;
using seabass::infrastructure::local::readRekordboxBaseline;
using seabass::infrastructure::local::readRekordboxBaselineSequence;
using seabass::infrastructure::local::writeRekordboxBaseline;
namespace paths = seabass::infrastructure::paths;
namespace hashing = seabass::infrastructure::hashing;

namespace
{

const auto NoHook = [](const std::string &) {};

fs::path freshStick(const std::string &name)
{
    const fs::path root = seabass::testing::scratchRoot() / "rekordbox_baseline_file_test" / pathFromUtf8(name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

std::string readBytes(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    assert(in);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeBytes(const fs::path &file, const std::string &bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    assert(out);
}

std::string gzip(const std::string &text)
{
    z_stream s{};
    assert(deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::string out(deflateBound(&s, static_cast<uLong>(text.size())) + 32, '\0');
    s.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(text.data()));
    s.avail_in = static_cast<uInt>(text.size());
    s.next_out = reinterpret_cast<Bytef *>(out.data());
    s.avail_out = static_cast<uInt>(out.size());
    assert(deflate(&s, Z_FINISH) == Z_STREAM_END);
    out.resize(s.total_out);
    deflateEnd(&s);
    return out;
}

std::string gunzip(const std::string &bytes)
{
    z_stream s{};
    assert(inflateInit2(&s, 15 + 16) == Z_OK);
    std::string out;
    char buffer[65536];
    s.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(bytes.data()));
    s.avail_in = static_cast<uInt>(bytes.size());
    int result = Z_OK;
    while (result != Z_STREAM_END) {
        s.next_out = reinterpret_cast<Bytef *>(buffer);
        s.avail_out = sizeof buffer;
        result = inflate(&s, Z_NO_FLUSH);
        assert(result == Z_OK || result == Z_STREAM_END);
        out.append(buffer, sizeof buffer - s.avail_out);
    }
    inflateEnd(&s);
    return out;
}

// A body with a correct #sha256 line, so a test can reach the parser with
// a file that is wrong only in the way it means to be.
std::string sealed(const std::string &body)
{
    return body + "#sha256\t" + hashing::toHex(hashing::Sha256::of(body)) + "\n";
}

void plantText(const fs::path &stick, const std::string &text)
{
    writeBytes(paths::stickRekordboxBaseline(stick), gzip(text));
}

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

CuePoint cue(CuePoint::Kind kind, int pad, double ms, const std::string &colour, const std::string &comment,
             bool loop = false, double endMs = 0.0)
{
    CuePoint c;
    c.kind = kind;
    c.hotCueNumber = pad;
    c.positionMs = ms;
    c.color = colour;
    c.comment = comment;
    c.isLoop = loop;
    c.loopEndMs = endMs;
    return c;
}

// Every field of every type, every escaping case, and the shapes the
// plan calls out: an empty playlist, folders, a member listed twice, two
// rows for one file, Unicode paths.
RekordboxBaseline everything()
{
    RekordboxBaseline b;
    b.pdbSequence = std::numeric_limits<std::uint64_t>::max();
    b.engineUuid = "6f1c\\9a2e\t-uuid";
    b.recordedAtUnix = -1234567890123;  // a negative clock must survive as well
    b.writer = "Seabass 0.8.9\nbuilt by\\hand";

    BaselineTrack t1;
    t1.pathKey = "contents/björk/jóga ☃.mp3";
    t1.stickRelativePath = "Contents/Björk/Jóga ☃.mp3";
    t1.analysisFile = "/PIONEER/USBANLZ/P016/0000875E/ANLZ0000.DAT";
    t1.pdbId = std::numeric_limits<std::uint32_t>::max();
    t1.rating = 5;
    t1.ratingOrigin = ValueOrigin::Seabass;
    t1.comment = "tab\there\nnewline\\backslash \\t not a tab";
    t1.bpm = 123.45678901234567;
    t1.durationMs = 245123;
    t1.cues = {
        BaselineCue{cue(CuePoint::Kind::Hot, 1, 1234.5678901234, "#FF0000", "drop\there"), ValueOrigin::Rekordbox},
        BaselineCue{cue(CuePoint::Kind::Hot, 2, 60000.1, "", "", true, 64000.000001), ValueOrigin::Seabass},
        BaselineCue{cue(CuePoint::Kind::Memory, 0, 0.0, "pink\\", "a\nb"), ValueOrigin::Unknown},
        BaselineCue{cue(CuePoint::Kind::Memory, 7, 1e-9, "", "", true, 1e15), ValueOrigin::Seabass},
    };

    BaselineTrack t2;
    t2.pathKey = "contents/a:b\\c\td.wav";  // a ':' needs nothing, the rest escaping
    t2.stickRelativePath = "Contents/a:b\\c\td.wav";
    t2.pdbId = 0;
    t2.rating = std::nullopt;
    t2.ratingOrigin = ValueOrigin::Unknown;
    t2.bpm = 0.0;
    t2.durationMs = 0;

    BaselineTrack t3 = t1;  // two rows for one file are both kept
    t3.pdbId = 17;
    t3.rating = 0;
    t3.ratingOrigin = ValueOrigin::Rekordbox;
    t3.comment.clear();
    t3.bpm = -0.0;
    t3.cues = {};

    b.tracks = {t1, t2, t3};

    BaselinePlaylist folder{1, 0, true, "Sets", {}};
    BaselinePlaylist list{2, 1, false, "Sets/Ünïcode ☃\tList\\x", {t1.pathKey, t2.pathKey, t1.pathKey}};
    BaselinePlaylist empty{3, 0, false, "Empty", {}};
    BaselinePlaylist nested{40, 1, true, "Sets/Sub", {}};
    BaselinePlaylist later{5, 40, false, "Sets/Sub/Later", {t2.pathKey}};
    b.playlists = {folder, list, empty, nested, later};

    b.declined = {
        {"cue:hot:1:" + t1.pathKey, "0123456789abcdef"},
        {"comment:" + t2.pathKey, "x\\y\tz\n"},
        {"playlist:3", ""},
    };
    return b;
}

void assertSameCue(const BaselineCue &a, const BaselineCue &b)
{
    assert(a.origin == b.origin);
    assert(a.cue.kind == b.cue.kind);
    assert(a.cue.hotCueNumber == b.cue.hotCueNumber);
    assert(a.cue.positionMs == b.cue.positionMs);
    assert(a.cue.color == b.cue.color);
    assert(a.cue.comment == b.cue.comment);
    assert(a.cue.isLoop == b.cue.isLoop);
    assert(a.cue.loopEndMs == b.cue.loopEndMs);
}

void assertSameBaseline(const RekordboxBaseline &a, const RekordboxBaseline &b)
{
    assert(a.pdbSequence == b.pdbSequence);
    assert(a.engineUuid == b.engineUuid);
    assert(a.recordedAtUnix == b.recordedAtUnix);
    assert(a.writer == b.writer);
    assert(a.tracks.size() == b.tracks.size());
    for (std::size_t i = 0; i < a.tracks.size(); ++i) {
        const BaselineTrack &x = a.tracks[i];
        const BaselineTrack &y = b.tracks[i];
        assert(x.pathKey == y.pathKey);
        assert(x.stickRelativePath == y.stickRelativePath);
        assert(x.analysisFile == y.analysisFile);
        assert(x.pdbId == y.pdbId);
        assert(x.rating == y.rating);
        assert(x.ratingOrigin == y.ratingOrigin);
        assert(x.comment == y.comment);
        assert(x.bpm == y.bpm);
        assert(std::signbit(x.bpm) == std::signbit(y.bpm));
        assert(x.durationMs == y.durationMs);
        assert(x.cues.size() == y.cues.size());
        for (std::size_t c = 0; c < x.cues.size(); ++c) {
            assertSameCue(x.cues[c], y.cues[c]);
        }
    }
    assert(a.playlists.size() == b.playlists.size());
    for (std::size_t i = 0; i < a.playlists.size(); ++i) {
        assert(a.playlists[i].id == b.playlists[i].id);
        assert(a.playlists[i].parentId == b.playlists[i].parentId);
        assert(a.playlists[i].folder == b.playlists[i].folder);
        assert(a.playlists[i].path == b.playlists[i].path);
        assert(a.playlists[i].members == b.playlists[i].members);
    }
    assert(a.declined == b.declined);
}

void testMissingFileIsNoBaselineAndNoError()
{
    const fs::path stick = freshStick("missing");
    std::string error = "stale";
    assert(!readRekordboxBaseline(stick, &error));
    assert(error.empty());
    error = "stale";
    assert(!readRekordboxBaselineSequence(stick, &error));
    assert(error.empty());
    // Not even the Seabass directory is created by a read.
    assert(!fs::exists(paths::stickDir(stick)));
}

void testPathIsBesideTheCachesNotInThem()
{
    const fs::path stick = pathFromUtf8("/stick");
    assert(paths::stickRekordboxBaseline(stick) == paths::stickDir(stick) / "rekordbox-baseline.tsv.gz");
    assert(paths::stickRekordboxBaseline(stick).parent_path() != paths::stickCachesDir(stick));
}

void testRoundTripKeepsEveryField()
{
    const fs::path stick = freshStick("round trip ☃");
    const RekordboxBaseline written = everything();
    std::string error;
    assert(writeRekordboxBaseline(stick, written, NoHook, &error));
    assert(error.empty());

    // A real gzip file, readable without Seabass.
    const std::string bytes = readBytes(paths::stickRekordboxBaseline(stick));
    assert(bytes.size() > 2 && static_cast<unsigned char>(bytes[0]) == 0x1f
           && static_cast<unsigned char>(bytes[1]) == 0x8b);
    const std::string text = gunzip(bytes);
    assert(text.rfind("seabass-rekordbox-baseline\t1\t18446744073709551615\t", 0) == 0);
    // Escaping as BackupManifest does it: one backslash doubled, tab and
    // newline spelled out, so no raw tab or newline leaks into a field.
    assert(contains(text, "\tdrop\\there\n"));
    assert(contains(text, "tab\\there\\nnewline\\\\backslash \\\\t not a tab"));

    error = "stale";
    const std::optional<RekordboxBaseline> read = readRekordboxBaseline(stick, &error);
    if (!read) {
        std::cerr << "round trip refused: " << error << "\n";
    }
    assert(read);
    assert(error.empty());
    assertSameBaseline(written, *read);

    // Writing what was read gives the same bytes before compression.
    const fs::path again = freshStick("round trip again");
    assert(writeRekordboxBaseline(again, *read, NoHook, &error));
    assert(gunzip(readBytes(paths::stickRekordboxBaseline(again))) == text);
}

void testSequenceReadAgreesAndReadsOnlyTheHeader()
{
    const fs::path stick = freshStick("sequence");
    RekordboxBaseline b = everything();
    b.pdbSequence = 4711;
    std::string error;
    assert(writeRekordboxBaseline(stick, b, NoHook, &error));
    error = "stale";
    const std::optional<std::uint64_t> sequence = readRekordboxBaselineSequence(stick, &error);
    assert(sequence && *sequence == 4711 && error.empty());
    assert(readRekordboxBaseline(stick, &error)->pdbSequence == *sequence);

    // An intact header over a damaged body: the badge's cheap read
    // answers (it does not verify the hash), the full read refuses.
    plantText(stick, "seabass-rekordbox-baseline\t1\t99\tuuid\t0\tw\nt\tgarbage\n#sha256\t00\n");
    error = "stale";
    assert(readRekordboxBaselineSequence(stick, &error) == std::optional<std::uint64_t>(99));
    assert(error.empty());
    assert(!readRekordboxBaseline(stick, &error));
    assert(!error.empty());
}

void testHashMismatchIsRefusedNamingTheFile()
{
    const fs::path stick = freshStick("sha mismatch");
    std::string error;
    assert(writeRekordboxBaseline(stick, everything(), NoHook, &error));
    const fs::path file = paths::stickRekordboxBaseline(stick);
    std::string text = gunzip(readBytes(file));
    const std::size_t at = text.find("\t245123\n");
    assert(at != std::string::npos);
    text.replace(at, 8, "\t245124\n");  // one digit of one duration
    writeBytes(file, gzip(text));

    error.clear();
    assert(!readRekordboxBaseline(stick, &error));
    assert(contains(error, pathToUtf8(file)));
    assert(contains(error, "#sha256"));
    std::cout << "sha mismatch: " << error << "\n";
}

void testTruncatedFileIsRefused()
{
    const fs::path stick = freshStick("truncated");
    std::string error;
    assert(writeRekordboxBaseline(stick, everything(), NoHook, &error));
    const fs::path file = paths::stickRekordboxBaseline(stick);
    const std::string bytes = readBytes(file);

    // Cut the compressed file short.
    writeBytes(file, bytes.substr(0, bytes.size() / 2));
    error.clear();
    assert(!readRekordboxBaseline(stick, &error));
    assert(contains(error, pathToUtf8(file)) && contains(error, "truncated"));
    std::cout << "truncated: " << error << "\n";

    // Only the gzip trailer missing: every byte of text is there, the
    // stream is still incomplete.
    writeBytes(file, bytes.substr(0, bytes.size() - 4));
    error.clear();
    assert(!readRekordboxBaseline(stick, &error));
    assert(!error.empty());

    // Intact gzip around text that stops before its #sha256 line.
    const std::string text = gunzip(bytes);
    plantText(stick, text.substr(0, text.rfind("#sha256")));
    error.clear();
    assert(!readRekordboxBaseline(stick, &error));
    assert(contains(error, "#sha256"));

    // An empty file.
    writeBytes(file, "");
    error.clear();
    assert(!readRekordboxBaseline(stick, &error));
    assert(!error.empty());
    error.clear();
    assert(!readRekordboxBaselineSequence(stick, &error));
    assert(!error.empty());

    // Bytes after the end of the compressed data.
    writeBytes(file, bytes + "x");
    error.clear();
    assert(!readRekordboxBaseline(stick, &error));
    assert(contains(error, "after the end"));
}

void testUnknownVersionIsRefused()
{
    const fs::path stick = freshStick("version");
    plantText(stick, sealed("seabass-rekordbox-baseline\t2\t5\tuuid\t0\tw\n"));
    std::string error;
    assert(!readRekordboxBaseline(stick, &error));
    assert(contains(error, "version 2"));
    std::cout << "version: " << error << "\n";
    error.clear();
    assert(!readRekordboxBaselineSequence(stick, &error));
    assert(contains(error, "version 2"));

    plantText(stick, sealed("not-a-baseline\t1\t5\tuuid\t0\tw\n"));
    error.clear();
    assert(!readRekordboxBaseline(stick, &error));
    assert(!error.empty());

    // A sequence that is not plain decimal.
    plantText(stick, sealed("seabass-rekordbox-baseline\t1\t0x10\tuuid\t0\tw\n"));
    error.clear();
    assert(!readRekordboxBaselineSequence(stick, &error));
    assert(contains(error, "decimal"));
}

void testUnknownLineKindAndMalformedFieldsAreRefused()
{
    const fs::path stick = freshStick("malformed");
    const std::string header = "seabass-rekordbox-baseline\t1\t5\tuuid\t0\tw\n";
    const std::string track = "t\tk\tK\t1\t\t-\t?\t\t120\t1000\n";
    struct Case
    {
        std::string body;
        std::string expected;
    };
    const std::vector<Case> cases = {
        {header + "z\tsomething\n", "unknown line kind"},
        {header + "\n", "unknown line kind"},
        {header + track + "c\thot\t1\t10\t0\t0\t\tq\t\n", "malformed cue line"},  // origin letter q
        {header + track + "c\thot\t1\t10\t0\t0\t\trs\t\n", "malformed cue line"},
        {header + track + "c\tsideways\t1\t10\t0\t0\t\tr\t\n", "malformed cue line"},
        {header + "t\tk\tK\t1\t\t-\tx\t\t120\t1000\n", "malformed track line"},  // rating origin x
        {header + "t\tk\tK\t1\t\t-\t?\t\tabc\t1000\n", "malformed track line"},
        {header + "t\tk\tK\t1\t\t-\t?\t\tnan\t1000\n", "malformed track line"},
        {header + "t\tk\tK\t1\t\t-\t?\t\t120\n", "malformed track line"},  // a field short
        {header + "t\t\tK\t1\t\t-\t?\t\t120\t1000\n", "malformed track line"},  // no pathKey
        {header + "t\tk\tK\t1\t\tfive\t?\t\t120\t1000\n", "rating"},
        {header + "t\tk\\q\tK\t1\t\t-\t?\t\t120\t1000\n", "malformed track line"},  // unknown escape
        {header + "c\thot\t1\t10\t0\t0\t\tr\t\n", "no track line"},
        {header + "p\t1\t0\t2\tX\n", "malformed playlist line"},  // folder flag 2
        {header + "p\t1\t0\t0\tX\np\t1\t0\t0\tY\n", "listed twice"},
        {header + "p\t1\t0\t0\tX\n" + track + "m\t1\t1\tk\n", "position 1"},
        {header + "m\t9\t0\tk\n", "no playlist line"},
        {header + track + "p\t1\t0\t0\tX\n", "after a later section"},
        {header + "x\tkey\n", "malformed declined line"},
    };
    for (const Case &c : cases) {
        plantText(stick, sealed(c.body));
        std::string error;
        const bool refused = !readRekordboxBaseline(stick, &error);
        if (!refused || !contains(error, c.expected)) {
            std::cerr << "case not refused as expected (" << c.expected << "): " << error << "\n";
        }
        assert(refused);
        assert(contains(error, c.expected));
        assert(contains(error, pathToUtf8(paths::stickRekordboxBaseline(stick))));
    }
    // The control: the same pieces, well formed, read.
    plantText(stick, sealed(header + "p\t1\t0\t0\tX\n" + track + "c\thot\t1\t10\t0\t0\t\ts\t\nm\t1\t0\tk\nx\tkey\t\n"));
    std::string error;
    const auto ok = readRekordboxBaseline(stick, &error);
    assert(ok && error.empty());
    assert(ok->tracks.size() == 1 && ok->tracks[0].cues.size() == 1);
    assert(ok->tracks[0].cues[0].origin == ValueOrigin::Seabass);
    assert(ok->playlists[0].members == std::vector<std::string>{"k"});
    assert(ok->declined.at("key").empty());
}

void testNullErrorAndEmptyHookThrow()
{
    const fs::path stick = freshStick("null");
    bool threw = false;
    try {
        readRekordboxBaseline(stick, nullptr);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    assert(threw);
    threw = false;
    try {
        readRekordboxBaselineSequence(stick, nullptr);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    assert(threw);
    threw = false;
    try {
        writeRekordboxBaseline(stick, everything(), NoHook, nullptr);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    assert(threw);
    threw = false;
    std::string error;
    try {
        writeRekordboxBaseline(stick, everything(), {}, &error);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    assert(threw);
    assert(!fs::exists(paths::stickRekordboxBaseline(stick)));
}

void testBeforeWriteNamesTheFileBeforeItChanges()
{
    const fs::path stick = freshStick("before write");
    const fs::path file = paths::stickRekordboxBaseline(stick);
    std::vector<std::string> named;
    std::string error;

    // First write: no Seabass directory yet, and none before the hook.
    assert(writeRekordboxBaseline(
        stick, everything(),
        [&](const std::string &path) {
            named.push_back(path);
            assert(!fs::exists(file));
        },
        &error));
    assert(named == std::vector<std::string>{pathToUtf8(file)});

    // Second write: the hook sees the old file, whole.
    const std::string before = readBytes(file);
    RekordboxBaseline next = everything();
    next.pdbSequence = 1;
    assert(writeRekordboxBaseline(
        stick, next,
        [&](const std::string &path) {
            named.push_back(path);
            assert(readBytes(pathFromUtf8(path)) == before);
        },
        &error));
    assert(named.size() == 2 && named[1] == pathToUtf8(file));
    assert(readRekordboxBaselineSequence(stick, &error) == std::optional<std::uint64_t>(1));
}

void testRefusedWritesLeaveTheOldFile()
{
    const fs::path stick = freshStick("refused");
    const fs::path file = paths::stickRekordboxBaseline(stick);
    std::string error;
    assert(writeRekordboxBaseline(stick, everything(), NoHook, &error));
    const std::string before = readBytes(file);
    int hookCalls = 0;
    const auto countingHook = [&](const std::string &) { ++hookCalls; };

    // A baseline that cannot be written as it is: refused before the hook.
    RekordboxBaseline twoIds = everything();
    twoIds.playlists[2].id = twoIds.playlists[1].id;
    assert(!writeRekordboxBaseline(stick, twoIds, countingHook, &error));
    assert(contains(error, "two playlists"));
    RekordboxBaseline noKey = everything();
    noKey.tracks[1].pathKey.clear();
    assert(!writeRekordboxBaseline(stick, noKey, countingHook, &error));
    RekordboxBaseline nanCue = everything();
    nanCue.tracks[0].cues[0].cue.positionMs = std::numeric_limits<double>::quiet_NaN();
    assert(!writeRekordboxBaseline(stick, nanCue, countingHook, &error));
    assert(hookCalls == 0);
    assert(readBytes(file) == before);

    // A stick root that is not there: never created.
    const fs::path gone = stick / "unmounted";
    assert(!writeRekordboxBaseline(gone, everything(), countingHook, &error));
    assert(contains(error, "not a directory"));
    assert(!fs::exists(gone));
    assert(hookCalls == 0);

#if !defined(_WIN32)
    // The write itself fails (a directory this process cannot write;
    // root ignores permissions, so the check is skipped there). Whether a
    // half-written temp file can ever appear at the path is
    // durable_file_write_test's business; here: false, an error, and the
    // old file as it was.
    if (::geteuid() != 0) {
        fs::permissions(paths::stickDir(stick), fs::perms::owner_read | fs::perms::owner_exec);
        RekordboxBaseline next = everything();
        next.pdbSequence = 2;
        const bool written = writeRekordboxBaseline(stick, next, countingHook, &error);
        fs::permissions(paths::stickDir(stick), fs::perms::owner_all);
        assert(!written);
        assert(contains(error, "unchanged"));
        assert(hookCalls == 1);
        assert(readBytes(file) == before);
        for (const auto &entry : fs::directory_iterator(paths::stickDir(stick))) {
            assert(entry.path().filename() == file.filename());
        }
    }
#endif
}

// Not asserted: printed so the plan's size estimate can be checked
// against a baseline with text that does not compress like placeholders.
void printSizeForFifteenHundredTracks()
{
    const fs::path stick = freshStick("size");
    RekordboxBaseline b;
    b.pdbSequence = 123456;
    b.engineUuid = "1b6c2f9e-5d0a-4f3e-9a51-7c3d2e8f4b10";
    b.recordedAtUnix = 1791504000;
    b.writer = "Seabass 0.8.9";
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    const auto next = [&]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    const auto word = [&](int length) {
        static const char Letters[] = "abcdefghijklmnopqrstuvwxyz";
        std::string w;
        for (int i = 0; i < length; ++i) {
            w.push_back(Letters[next() % 26]);
        }
        return w;
    };
    for (int i = 0; i < 1500; ++i) {
        BaselineTrack t;
        t.stickRelativePath = "Contents/" + word(8) + " " + word(6) + "/" + word(7) + " " + word(9) + "/"
                              + std::to_string(i % 20 + 1) + " " + word(10) + " " + word(5) + ".mp3";
        t.pathKey = t.stickRelativePath;
        for (char &c : t.pathKey) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        t.analysisFile = "/PIONEER/USBANLZ/P0" + std::to_string(10 + next() % 90) + "/000"
                         + std::to_string(10000 + next() % 89999) + "/ANLZ0000.DAT";
        t.pdbId = static_cast<std::uint32_t>(i + 1);
        t.rating = static_cast<int>(next() % 6);
        t.ratingOrigin = ValueOrigin::Unknown;
        t.comment = i % 3 == 0 ? word(12) + " " + word(8) : "";
        t.bpm = 110.0 + static_cast<double>(next() % 3000) / 100.0;
        t.durationMs = 180000 + static_cast<std::int64_t>(next() % 240000);
        const int hot = static_cast<int>(next() % 6);
        for (int pad = 1; pad <= hot; ++pad) {
            t.cues.push_back(BaselineCue{cue(CuePoint::Kind::Hot, pad, static_cast<double>(next() % 300000) + 0.25,
                                             "#28E214", ""),
                                         ValueOrigin::Unknown});
        }
        for (int m = 0; m < 3; ++m) {
            t.cues.push_back(BaselineCue{
                cue(CuePoint::Kind::Memory, 0, static_cast<double>(next() % 300000) + 0.5, "", ""),
                ValueOrigin::Unknown});
        }
        b.tracks.push_back(std::move(t));
    }
    for (std::uint32_t id = 1; id <= 33; ++id) {
        BaselinePlaylist p{id, 0, false, word(6) + " " + word(8), {}};
        for (int m = 0; m < 63; ++m) {
            p.members.push_back(b.tracks[next() % b.tracks.size()].pathKey);
        }
        b.playlists.push_back(std::move(p));
    }
    std::string error;
    assert(writeRekordboxBaseline(stick, b, NoHook, &error));
    const std::string bytes = readBytes(paths::stickRekordboxBaseline(stick));
    const std::string text = gunzip(bytes);
    std::cout << "1500 tracks, 33 playlists, 2079 entries: " << text.size() << " bytes raw, " << bytes.size()
              << " bytes deflated\n";
    const auto read = readRekordboxBaseline(stick, &error);
    assert(read && read->tracks.size() == 1500);
}

}  // namespace

int main()
{
    testPathIsBesideTheCachesNotInThem();
    testMissingFileIsNoBaselineAndNoError();
    testRoundTripKeepsEveryField();
    testSequenceReadAgreesAndReadsOnlyTheHeader();
    testHashMismatchIsRefusedNamingTheFile();
    testTruncatedFileIsRefused();
    testUnknownVersionIsRefused();
    testUnknownLineKindAndMalformedFieldsAreRefused();
    testNullErrorAndEmptyHookThrow();
    testBeforeWriteNamesTheFileBeforeItChanges();
    testRefusedWritesLeaveTheOldFile();
    printSizeForFifteenHundredTracks();
    std::cout << "rekordbox_baseline_file_test: all passed\n";
    return 0;
}
