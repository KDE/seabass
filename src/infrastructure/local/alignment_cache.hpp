// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "domain/audio_alignment_probe.hpp"

namespace seabass::infrastructure::local
{

// A stick-local record of how far apart pairs of copies of one song are,
// so lining them up costs two full decodes once per stick rather than on
// every scan. JSON Lines at "<stickRoot>/Seabass/caches/alignment.jsonl",
// beside the silence cache and shaped like it, one line per PAIR:
//
//   {"a":"Contents/x/Song.mp3","amtime":"1710000000","asize":"8000000",
//    "b":"Contents/y/Song.flac","bmtime":"1710000500","bsize":"30000000",
//    "decoder":"ffmpeg qt6.8","method":"3","result":"shift","shift":"21.315000"}
//
// `result` is "shift" (the same music, `shift` ms later in b than in a),
// "no" (decoded whole, does not line up) or "same" (the same bytes). The
// shift is on the DECODER's timeline, as the probe measures it: the
// offsets that turn it into a shift between cues depend on a setting and
// on each file's own header, and are applied where it is used.
//
// Paths are relative to the stick root. A pair is stored once, `a` the
// smaller path; asked the other way round, the shift is negated. An
// entry is trusted only while BOTH files' size and mtime still match
// what was stored, the strictness of the silence and duration caches:
// this feeds cue writes.
//
// Never stored: no opinion (no decoder, a file that would not decode, a
// timeout, a cancelled scan). That is about the moment, not the files.
//
// `decoder` names the decoder that measured (its backend and version):
// another one may start a file a few milliseconds elsewhere (an MP3's or
// AAC's priming), so a line is trusted only by the decoder that wrote it.
// Lines of other decoders are kept, for the machine that wrote them.
//
// `method` is MethodVersion when the line was written. A line written by
// another version of the measurement is ignored and dropped at the next
// save: a better method may give another answer for the same pair.
//
// A cache that cannot be read (garbage, a folder where the file should
// be) is an empty cache, and one that cannot be written (a read-only
// stick) is only time lost: neither ever fails a scan.
class CachedAudioAlignmentProbe : public domain::AudioAlignmentProbe
{
public:
    // Version 2: lags picked by the correlation coefficient, rivals one
    // bar or beat away make a window unknown, the search bounded.
    // Version 3: rivals at 0.95, and a file too short for three windows of
    // its own checked in half-second parts.
    static constexpr int MethodVersion = 3;

    // `inner` may be null: then only what is cached is answered. A build
    // without a decoder still reads what a build with one wrote.
    // `innerDecodes` says whether `inner` can decode audio at all (as
    // opposed to comparing bytes), for hasDecoder().
    // `decoder` names the decoder behind `inner` (see above).
    CachedAudioAlignmentProbe(std::string stickRoot, std::unique_ptr<domain::AudioAlignmentProbe> inner,
                              bool innerDecodes, std::string decoder);
    ~CachedAudioAlignmentProbe() override;
    CachedAudioAlignmentProbe(const CachedAudioAlignmentProbe &) = delete;
    CachedAudioAlignmentProbe &operator=(const CachedAudioAlignmentProbe &) = delete;

    Answer align(const std::string &absolutePathA, const std::string &absolutePathB) override;
    bool sameContent(const std::string &absolutePathA, const std::string &absolutePathB) override;
    bool answersAtOnce(const std::string &absolutePathA, const std::string &absolutePathB) override;

    // Writes the cache back atomically when anything changed. False when
    // it could not be written; a caller carries on. Also run by the
    // destructor.
    bool save();

    // Pairs loaded from disk and still held (for the tests).
    std::size_t size() const { return m_entries.size(); }
    bool hasDecoder() const { return m_innerDecodes; }

    // What align() answered, for the page to say: pairs lined up (by
    // measuring or from the cache), and pairs left unknown (not lined up,
    // or no opinion).
    int linedUpCount() const { return m_linedUp; }
    int unknownCount() const { return m_unknown; }
    // Answers that came from the cache file rather than from `inner`.
    int cachedAnswerCount() const { return m_fromCache; }

private:
    enum class Result { Shift, NotLinedUp, Same };
    struct FileState
    {
        long long sizeBytes = 0;
        long long mtimeSeconds = 0;
        bool operator==(const FileState &) const = default;
    };
    struct Entry
    {
        FileState a;
        FileState b;
        Result result = Result::Shift;
        double shiftMs = 0.0;  // Shift only: where a sound is in b minus in a
    };
    // A pair as it is asked, turned into the stored order.
    struct Lookup
    {
        std::pair<std::string, std::string> key;
        FileState a;  // of key.first
        FileState b;  // of key.second
        bool reversed = false;
        bool usable = false;  // both on the stick, both stat-able, two files
    };

    std::string relativeKey(const std::string &absoluteFilePath) const;
    Lookup lookup(const std::string &absolutePathA, const std::string &absolutePathB) const;
    const Entry *trusted(const Lookup &pair) const;
    void store(const Lookup &pair, Result result, double shiftAsAsked);

    std::string m_stickRoot;
    std::filesystem::path m_cachePath;
    // Keyed by decoder, then the pair.
    std::map<std::pair<std::string, std::pair<std::string, std::string>>, Entry> m_entries;
    std::string m_decoder;
    std::unique_ptr<domain::AudioAlignmentProbe> m_inner;
    bool m_innerDecodes = false;
    bool m_dirty = false;
    int m_linedUp = 0;
    int m_unknown = 0;
    int m_fromCache = 0;
};

}  // namespace seabass::infrastructure::local
