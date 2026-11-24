// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace seabass::infrastructure::engine
{

// The blob columns of an Engine row's performance data. 2.x keeps them in
// Track, 3.x in PerformanceData (both in Database2/m.db), 1.x in p.db's
// PerformanceData, the high-resolution waveform with them.
enum class EngineBlob : unsigned {
    TrackData = 1u << 0,
    BeatData = 1u << 1,
    OverviewWaveFormData = 1u << 2,
    HighResolutionWaveFormData = 1u << 3,
    QuickCues = 1u << 4,
    Loops = 1u << 5,
};

// A set of EngineBlob values, as a bit mask; 0 is none.
using EngineBlobs = unsigned;

constexpr EngineBlobs operator|(EngineBlob a, EngineBlob b)
{
    return static_cast<EngineBlobs>(a) | static_cast<EngineBlobs>(b);
}
constexpr EngineBlobs operator|(EngineBlobs a, EngineBlob b)
{
    return a | static_cast<EngineBlobs>(b);
}
constexpr bool contains(EngineBlobs set, EngineBlob blob)
{
    return (set & static_cast<EngineBlobs>(blob)) != 0;
}

// What libdjinterop decodes for each of its track calls, so a caller asks
// only about the blobs the call it makes needs.
namespace engine_blobs_for
{
// sample_rate(), sample_count(), set_key(), the average loudness.
constexpr EngineBlobs trackData = static_cast<EngineBlobs>(EngineBlob::TrackData);
// hot_cues(), main_cue(), set_hot_cues(), set_main_cue().
constexpr EngineBlobs quickCues = static_cast<EngineBlobs>(EngineBlob::QuickCues);
// loops().
constexpr EngineBlobs loops = static_cast<EngineBlobs>(EngineBlob::Loops);
// waveform(): the overview on 2.x and 3.x, the high-resolution one on 1.x.
constexpr EngineBlobs waveform = EngineBlob::OverviewWaveFormData | EngineBlob::HighResolutionWaveFormData;
// set_sample_rate() rewrites both records the rate is in; on 1.x both
// waveforms too.
constexpr EngineBlobs setSampleRate =
    EngineBlob::TrackData | EngineBlob::BeatData | EngineBlob::OverviewWaveFormData | EngineBlob::HighResolutionWaveFormData;
// snapshot(): every blob of the row.
constexpr EngineBlobs everything = setSampleRate | EngineBlob::QuickCues | EngineBlob::Loops;
}  // namespace engine_blobs_for

// The column names of `blobs`, as the schema spells them, comma separated:
// "beatData, quickCues". For a note that names what was left unread.
std::string engineBlobNames(EngineBlobs blobs);

namespace internal
{

// Whether libdjinterop's zlib_uncompress can be handed `bytes` safely: a
// four-byte big-endian length, then a zlib stream. As pinned, it takes the
// address of byte 4 even when the blob has only four (out of range: an
// abort under MSVC's checked iterators that no catch sees), gives inflate
// four bytes more than the blob holds, and loops for ever on a stream that
// does not end within them (inflate then returns Z_BUF_ERROR with no input
// left, which it does not treat as the end). It also reserves the declared
// length up front and inflates without a bound. So a blob is passed on
// only when it is empty or says length 0 (both decode to nothing without a
// read past the header), or when its declared length is within a sane
// bound, and its stream, inflated here first, ends within the blob and
// inflates to exactly the length declared. Column-blind: the bound is the
// largest any column has (EngineBlob::HighResolutionWaveFormData's). For
// a row's column, the overload below.
bool engineBlobDecodable(std::string_view bytes);

// The same for one column, with that column's own bound on the size and,
// once inflated (loops are stored as they are), the counts its decoder
// sizes or loops by checked against the bytes there are:
//   - trackData: a fixed record, 4 KiB at most;
//   - beatData: both beat grids, each count from 0 to one marker every
//     50 ms of the samples the record says the track has (no more than
//     the bytes there are when it gives no usable rate and length), 24
//     bytes a marker, each grid within the record, the second count's own
//     8 bytes too; 16 MiB at most. libdjinterop's 1.x decoder refuses a
//     grid of more than 32768 markers itself;
//   - overviewWaveFormData, highResolutionWaveFormData: the point count
//     from 0 to what the record holds at 3 (6) bytes a point; 1 MiB for
//     an overview, 64 MiB for the high-resolution waveform;
//   - quickCues: 0 to 8 hot cues (the player's eight pads), each at least
//     13 bytes, 64 KiB at most;
//   - loops: 0 to 8 loops, each at least 23 bytes, 64 KiB at most.
// The declared length must match what the stream inflates to. A record
// shorter than its decoder's minimum passes: libdjinterop refuses that
// one itself, with an exception, before it reads a count.
bool engineBlobDecodable(EngineBlob column, std::string_view bytes);

}  // namespace internal

// Which of an Engine library's rows hold a blob libdjinterop must not be
// handed (internal::engineBlobDecodable), read straight from the database
// read-only: Database2/m.db on 2.x and 3.x, p.db on 1.x. A caller asks
// before a libdjinterop call that decodes a blob, and leaves the call out
// (and says so) for a row whose blob it needs is damaged: libdjinterop
// hangs on some damaged blobs, which no catch gets back from.
//
// The connection is opened on first use and held; every read is its own
// statement, reset after, so no lock is held between calls. Not
// thread-safe.
class EngineBlobGuard
{
public:
    explicit EngineBlobGuard(std::string engineLibraryPath);
    ~EngineBlobGuard();
    EngineBlobGuard(const EngineBlobGuard &) = delete;
    EngineBlobGuard &operator=(const EngineBlobGuard &) = delete;

    // The damaged blobs of one row (0 for none, and for a row the table
    // does not have). nullopt with *error when the row cannot be read.
    [[nodiscard]] std::optional<EngineBlobs> damaged(std::int64_t trackId, std::string *error);

    // Every row with a damaged blob, in one pass over the table. nullopt
    // with *error when the table cannot be read.
    [[nodiscard]] std::optional<std::unordered_map<std::int64_t, EngineBlobs>> damagedRows(std::string *error);

    // The connection and what was learnt of the schema (the .cpp's).
    struct State;

private:
    std::string m_engineLibraryPath;
    std::unique_ptr<State> m_state;
};

// Whether a guard's read failure leaves libdjinterop to decode unchecked,
// as before there was a check: true unless the database is locked or the
// read-only read failed on its journal or as read-only
// (SQLITE_READONLY_ROLLBACK: a hot journal, which a read-only connection
// cannot roll back), or could not open the file at all (SQLITE_CANTOPEN:
// a WAL database on a read-only mount, its log unread). A locked database
// is being written, and one with a hot journal was left mid-write: exactly
// when a blob may be half written, so the blobs are refused, never
// decoded unchecked. Any other
// failure lasts (a schema this check does not know), and there a library
// that reads as before is better than one that never reads at all.
bool engineBlobGuardFailureReadsUnchecked(const std::string &error);

// What a writer throws for a row it leaves alone because a blob it would
// have to decode is damaged: before anything of the row is written. A save
// skips the change that met it, with a note, and goes on with the rest.
class DamagedEngineRowError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

}  // namespace seabass::infrastructure::engine
