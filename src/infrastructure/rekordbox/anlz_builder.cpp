// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/anlz_builder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace
{

// The fixed sizes rekordbox 7 writes, the same in every file measured.
constexpr std::uint32_t FileHeaderLength = 28;
constexpr std::uint32_t PreviewColumns = 400;      // PWAV
constexpr std::uint32_t TinyPreviewColumns = 100;  // PWV2
constexpr std::uint32_t WidePreviewColumns = 1200; // PWV4, PWV6
constexpr std::uint32_t VbrIndexEntries = 400;     // PVBR, plus the sample count after them

// Scroll waveform byte = band level (WaveformEntry, sqrt(2) times RMS)
// times this, up to 127. Fitted through the origin against rekordbox's
// own PWV7 on 28 tracks with the same crossovers (RMS gains 136, 204 and
// 226, divided by sqrt(2)); correlation 0.84, 0.84, 0.85. rekordbox's
// curve is not a straight line (the low byte rises about 2.4 a dB), so
// quiet and loud passages are off by more than the middle.
constexpr std::array<double, 3> ScrollBandGain{96.0, 144.0, 160.0};
// PWV6 byte = the PWV7-scale value of the column's mean, times this.
// Fitted against the mean of rekordbox's own PWV7 over each column, 200
// tracks; correlation 0.96, 0.81, 0.74.
constexpr std::array<double, 3> ThreeBandPreviewGain{0.58, 0.65, 1.34};
// PWV3 and PWV5 height (5 bits) from the entry's peak: a straight line
// fitted on 28 tracks, correlation 0.93. rekordbox's heights stay at 0
// below about -12 dBFS, which the negative offset reproduces.
constexpr double ScrollHeightGain = 23.65;
constexpr double ScrollHeightOffset = -7.0;
// The tallest PWAV column is 25 on 195 of 200 tracks measured (never
// more), and the tallest PWV2 column 15, the top of its 4 bits; both
// look scaled to the track's own loudest column, which is what this does.
constexpr double PreviewHeightTop = 25.0;
constexpr double TinyPreviewHeightTop = 15.0;
// PWVC: three 16-bit values per track, 80 to 500. Their meaning is not
// known; this is the most common first value (1319 of 1732) and the most
// common second and third.
constexpr std::array<std::uint16_t, 3> WaveformColourValues{80, 100, 100};

void appendFourcc(std::string &out, const char *fourcc)
{
    out.append(fourcc, 4);
}

// A section: fourcc, len_header, len_tag, then `headerFields` (the rest of
// the header) and `body`.
std::string section(const char *fourcc, const std::string &headerFields, const std::string &body)
{
    std::string out;
    const auto lenHeader = static_cast<std::uint32_t>(12 + headerFields.size());
    appendFourcc(out, fourcc);
    appendU32BE(out, lenHeader);
    appendU32BE(out, static_cast<std::uint32_t>(lenHeader + body.size()));
    out += headerFields;
    out += body;
    return out;
}

std::string fileOf(const std::vector<std::string> &sections)
{
    std::string out;
    appendFourcc(out, "PMAI");
    appendU32BE(out, FileHeaderLength);
    appendU32BE(out, 0);  // len_file, set below
    appendU32BE(out, 1);
    appendU32BE(out, 0x00010000);
    appendU32BE(out, 0x00010000);
    appendU32BE(out, 0);
    for (const auto &s : sections) {
        out += s;
    }
    writeU32BE(out, 8, static_cast<std::uint32_t>(out.size()));
    return out;
}

// UTF-8 to UTF-16BE, refusing anything that is not UTF-8: a path a
// player cannot spell back is a track it cannot find.
std::string utf16BigEndian(const std::string &utf8)
{
    std::string out;
    std::size_t i = 0;
    const auto byteAt = [&](std::size_t k) { return static_cast<unsigned char>(utf8[k]); };
    while (i < utf8.size()) {
        const unsigned char lead = byteAt(i);
        std::uint32_t cp = 0;
        std::size_t length = 0;
        if (lead < 0x80) {
            cp = lead;
            length = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            cp = lead & 0x1F;
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            cp = lead & 0x0F;
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            cp = lead & 0x07;
            length = 4;
        } else {
            throw std::invalid_argument("track path is not UTF-8");
        }
        if (i + length > utf8.size()) {
            throw std::invalid_argument("track path is not UTF-8");
        }
        for (std::size_t k = 1; k < length; ++k) {
            if ((byteAt(i + k) & 0xC0) != 0x80) {
                throw std::invalid_argument("track path is not UTF-8");
            }
            cp = (cp << 6) | (byteAt(i + k) & 0x3F);
        }
        static constexpr std::array<std::uint32_t, 5> smallest{0, 0, 0x80, 0x800, 0x10000};
        if (cp < smallest[length] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            throw std::invalid_argument("track path is not UTF-8");
        }
        // The path ends at its first NUL for whoever reads it back.
        if (cp == 0) {
            throw std::invalid_argument("track path has a NUL in it");
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            appendU16BE(out, static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
            appendU16BE(out, static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            appendU16BE(out, static_cast<std::uint16_t>(cp));
        }
        i += length;
    }
    return out;
}

// PPTH: len_path counts the UTF-16BE bytes and the two of the NUL after
// them, the same in all three files (measured on every stick file).
std::string pathSection(const std::string &trackPath)
{
    std::string body = utf16BigEndian(trackPath);
    appendU16BE(body, 0);
    std::string fields;
    appendU32BE(fields, static_cast<std::uint32_t>(body.size()));
    return section("PPTH", fields, body);
}

// PVBR: a zero field, then 400 seek offsets and the sample count. 1620
// bytes in every file measured, whatever the format.
std::string vbrSection(const NewAnlzInput &input)
{
    std::string fields;
    appendU32BE(fields, 0);
    std::string body;
    for (std::uint32_t i = 0; i < VbrIndexEntries; ++i) {
        appendU32BE(body, input.mp3SeekOffsets.empty() ? 0 : input.mp3SeekOffsets[i]);
    }
    appendU32BE(body, input.mp3SampleCount);
    return section("PVBR", fields, body);
}

struct GridBeat
{
    std::uint16_t beatInBar = 1;
    std::uint16_t tempo = 0;  // BPM x 100
    std::uint32_t timeMs = 0;
    std::uint16_t microseconds = 0;  // the part of a millisecond PQTZ cannot hold
};

// Every beat as PQTZ and PQT2 hold it. The tempo is each beat's own: from
// the gap to the next beat, the last beat taking the gap before it, so a
// grid whose spacing changes says so beat by beat. (Every one of the
// 1830 measured rekordbox grids has one tempo throughout; they were made
// that way, the format does not ask it.)
std::vector<GridBeat> gridBeats(const std::vector<domain::Beat> &beats)
{
    std::vector<GridBeat> out;
    out.reserve(beats.size());
    for (std::size_t i = 0; i < beats.size(); ++i) {
        const double t = beats[i].timeMs;
        if (!std::isfinite(t) || t < 0.0 || t >= 4294967295.0) {
            throw std::invalid_argument("beat " + std::to_string(i) + " is not a time in the track");
        }
        if (i > 0 && !(t > beats[i - 1].timeMs)) {
            throw std::invalid_argument("beat " + std::to_string(i) + " is not after the beat before it");
        }
    }
    for (std::size_t i = 0; i < beats.size(); ++i) {
        GridBeat beat;
        const double t = beats[i].timeMs;
        double whole = std::floor(t);
        long micro = std::lround((t - whole) * 1000.0);
        if (micro >= 1000) {
            whole += 1.0;
            micro = 0;
        }
        beat.timeMs = static_cast<std::uint32_t>(whole);
        beat.microseconds = static_cast<std::uint16_t>(micro);

        double gap = 0.0;
        if (i + 1 < beats.size()) {
            gap = beats[i + 1].timeMs - t;
        } else if (i > 0) {
            gap = t - beats[i - 1].timeMs;
        }
        if (gap > 0.0) {
            const double tempo = std::round(6000000.0 / gap);
            if (tempo > 65535.0) {
                throw std::invalid_argument("beat " + std::to_string(i) + " is too close to the next for PQTZ");
            }
            beat.tempo = static_cast<std::uint16_t>(std::max(1.0, tempo));
        } else {
            // A grid of one beat has no spacing; 120 BPM says nothing
            // false about a single beat.
            beat.tempo = 12000;
        }

        const int given = beats[i].beatInBar;
        if (given >= 1 && given <= 4) {
            beat.beatInBar = static_cast<std::uint16_t>(given);
        } else {
            beat.beatInBar = out.empty() ? 1 : static_cast<std::uint16_t>(out.back().beatInBar % 4 + 1);
        }
        out.push_back(beat);
    }
    return out;
}

// PQTZ: header fields 0, 0x00080000 and the beat count (in every one of
// 1834 measured), then 8 bytes a beat: beat in bar, tempo, time in ms.
std::string beatGridSection(const std::vector<GridBeat> &beats)
{
    std::string fields;
    appendU32BE(fields, 0);
    appendU32BE(fields, 0x00080000);
    appendU32BE(fields, static_cast<std::uint32_t>(beats.size()));
    std::string body;
    for (const auto &b : beats) {
        appendU16BE(body, b.beatInBar);
        appendU16BE(body, b.tempo);
        appendU32BE(body, b.timeMs);
    }
    return section("PQTZ", fields, body);
}

// PQT2, measured on 1822 tracks, all agreeing: a 56-byte header of
// 0, 0x01000002, 0, the first and the last beat as PQTZ writes them
// (8 bytes each), the beat count, the sum of every PQTZ field of every
// beat (beat in bar + tempo + time, modulo 2^32: all 1822), and two
// zeros; then 2 bytes a beat, 0 to 999 (the largest seen is 997), which
// read as the beat's microseconds past PQTZ's whole millisecond: on a
// grid of 476.19 ms beats they step by 190 or 191 and wrap at 1000.
std::string extendedBeatGridSection(const std::vector<GridBeat> &beats)
{
    std::uint32_t sum = 0;
    for (const auto &b : beats) {
        sum += static_cast<std::uint32_t>(b.beatInBar) + b.tempo + b.timeMs;
    }
    const auto appendBeat = [](std::string &out, const GridBeat &b) {
        appendU16BE(out, b.beatInBar);
        appendU16BE(out, b.tempo);
        appendU32BE(out, b.timeMs);
    };
    std::string fields;
    appendU32BE(fields, 0);
    appendU32BE(fields, 0x01000002);
    appendU32BE(fields, 0);
    appendBeat(fields, beats.front());
    appendBeat(fields, beats.back());
    appendU32BE(fields, static_cast<std::uint32_t>(beats.size()));
    appendU32BE(fields, sum);
    appendU32BE(fields, 0);
    appendU32BE(fields, 0);
    std::string body;
    for (const auto &b : beats) {
        appendU16BE(body, b.microseconds);
    }
    return section("PQT2", fields, body);
}

std::uint8_t clampByte(double value, double top)
{
    if (!(value > 0.0)) {
        return 0;
    }
    return static_cast<std::uint8_t>(std::lround(std::min(value, top)));
}

// The 3-bit whiteness of PWAV, PWV3: rekordbox's tracks the share of the
// level that is not bass (correlation of PWV3's whiteness with PWV7's low
// share: -0.81 over 28 tracks), so this is that share in eighths.
std::uint8_t whiteness(const domain::WaveformEntry &e)
{
    const double total = static_cast<double>(e.low) + e.mid + e.high;
    if (total <= 0.0) {
        return 0;
    }
    return clampByte(7.0 * (static_cast<double>(e.mid) + e.high) / total, 7.0);
}

std::uint8_t scrollHeight(const domain::WaveformEntry &e)
{
    return clampByte(ScrollHeightGain * e.overall + ScrollHeightOffset, 31.0);
}

// Low, mid, high, in that order, in PWV7 and in PWV6. The public
// description of the format has them as mid, high, low; rekordbox 7's
// own files say otherwise: over 8 tracks of a private collection, byte
// 0 follows the audio's level below 150 Hz (correlation 0.87, against
// 0.12 and -0.02 for the other two bands), byte 1 the level from 150 Hz
// to 2 kHz (0.86) and byte 2 the level above 2 kHz (0.88).
std::array<std::uint8_t, 3> scrollBands(const domain::WaveformEntry &e)
{
    return {clampByte(e.low * ScrollBandGain[0], 127.0), clampByte(e.mid * ScrollBandGain[1], 127.0),
            clampByte(e.high * ScrollBandGain[2], 127.0)};
}

// The scroll waveform's entries, or as many silent ones as the length
// asks for when there is no audio measurement.
std::vector<domain::WaveformEntry> scrollEntries(const NewAnlzInput &input)
{
    // A day: far beyond any track or recorded set, far inside what the
    // 32-bit entry counts hold. A length beyond it is a mistake upstream
    // (a duration read wrongly from a tag), not a track.
    constexpr double LongestMs = 24.0 * 60.0 * 60.0 * 1000.0;
    constexpr auto MostEntries = static_cast<std::size_t>(LongestMs * domain::WaveformEntriesPerSecond / 1000.0);
    if (!input.waveform.entries.empty()) {
        if (input.waveform.entries.size() > MostEntries) {
            throw std::invalid_argument("the waveform is longer than a day");
        }
        return input.waveform.entries;
    }
    if (!(input.durationMs > 0.0) || !std::isfinite(input.durationMs)) {
        throw std::invalid_argument("no waveform and no duration: the analysis would have no length");
    }
    if (input.durationMs > LongestMs) {
        throw std::invalid_argument("the duration is longer than a day");
    }
    const auto count = static_cast<std::size_t>(std::ceil(input.durationMs * domain::WaveformEntriesPerSecond / 1000.0));
    return std::vector<domain::WaveformEntry>(count);
}

// PWV3, PWV5, PWV7 share a 24-byte header: entry size, entry count, then
// 0x0096 (150 entries a second) and two bytes that are 0000 in PWV3 and
// PWV7 and 0305 in PWV5, in every file measured.
std::string scrollHeader(std::uint32_t entryBytes, std::uint32_t entries, std::uint16_t lastField)
{
    std::string fields;
    appendU32BE(fields, entryBytes);
    appendU32BE(fields, entries);
    appendU16BE(fields, 150);
    appendU16BE(fields, lastField);
    return fields;
}

// PWAV and PWV2: 5-bit (4-bit for PWV2) heights scaled to the loudest
// column, PWAV's with the whiteness above them.
std::string monochromePreview(const std::vector<domain::WaveformEntry> &entries, std::uint32_t columns, double top,
                              bool withWhiteness)
{
    const auto reduced = domain::reduceWaveform(entries, columns);
    float loudest = 0.0f;
    for (const auto &c : reduced) {
        loudest = std::max(loudest, c.overall);
    }
    std::string body;
    for (std::uint32_t j = 0; j < columns; ++j) {
        if (j >= reduced.size() || loudest <= 0.0f) {
            body.push_back(0);
            continue;
        }
        const std::uint8_t height = clampByte(top * reduced[j].overall / loudest, top);
        const std::uint8_t white = withWhiteness ? whiteness(reduced[j]) : 0;
        body.push_back(static_cast<char>(static_cast<std::uint8_t>(white << 5) | height));
    }
    return body;
}

std::string previewSection(const char *fourcc, const std::string &body, std::uint32_t columns)
{
    // len_preview, then 0x00010000, in every PWAV and PWV2 measured.
    std::string fields;
    appendU32BE(fields, columns);
    appendU32BE(fields, 0x00010000);
    return section(fourcc, fields, body);
}

}  // namespace

NewAnlzFiles buildAnlzFiles(const NewAnlzInput &input)
{
    if (input.trackPath.empty() || input.trackPath.front() != '/') {
        throw std::invalid_argument("track path must start with '/': \"" + input.trackPath + "\"");
    }
    if (!input.mp3SeekOffsets.empty() && input.mp3SeekOffsets.size() != VbrIndexEntries) {
        throw std::invalid_argument("an MP3 seek table has 400 entries, not " +
                                    std::to_string(input.mp3SeekOffsets.size()));
    }
    if (!input.waveform.entries.empty() && input.waveform.sampleRateHz > 0 && input.sampleRateHz > 0 &&
        input.waveform.sampleRateHz != input.sampleRateHz) {
        throw std::invalid_argument("the waveform was measured at another sample rate than the track's");
    }

    const std::string path = pathSection(input.trackPath);
    const std::vector<GridBeat> beats = gridBeats(input.beats);
    const std::vector<domain::WaveformEntry> entries = scrollEntries(input);
    const auto scrollCount = static_cast<std::uint32_t>(entries.size());

    // Cue lists, empty, through the codecs the cue writer uses: a hot and
    // a memory list in each of .DAT and .EXT (PCOB), and in .EXT the
    // newer pair too (PCO2), hot first, as every real file has them. The
    // empty memory PCOB ends its header with 0xFFFFFFFF, "no last entry",
    // which is the end marker the XDJ-RX2 needs (#33); the codec sets it
    // from the list.
    const std::string hotLegacy = AnlzLegacyCueCodec::encodeCues({}, CueListTypeHot);
    const std::string memoryLegacy = AnlzLegacyCueCodec::encodeCues({}, CueListTypeMemory);
    const std::string hotCues = AnlzCueCodec::encodeHotCues({}, CueListTypeHot);
    const std::string memoryCues = AnlzCueCodec::encodeHotCues({}, CueListTypeMemory);

    // .DAT
    std::string datPreview = monochromePreview(entries, PreviewColumns, PreviewHeightTop, true);
    std::string datTiny = monochromePreview(entries, TinyPreviewColumns, TinyPreviewHeightTop, false);
    NewAnlzFiles files;
    files.dat = fileOf({path, vbrSection(input), beatGridSection(beats),
                        previewSection("PWAV", datPreview, PreviewColumns),
                        previewSection("PWV2", datTiny, TinyPreviewColumns), hotLegacy, memoryLegacy});

    // Scrolling waveforms: PWV3 (height and whiteness), PWV5 (colour and
    // height), PWV7 (three bands).
    std::string mono, colour, threeBand;
    mono.reserve(entries.size());
    colour.reserve(entries.size() * 2);
    threeBand.reserve(entries.size() * 3);
    for (const auto &e : entries) {
        const std::uint8_t height = scrollHeight(e);
        mono.push_back(static_cast<char>(static_cast<std::uint8_t>(whiteness(e) << 5) | height));
        // PWV5, a big-endian 16-bit word: red in bits 13 to 15, green 10
        // to 12, blue 7 to 9, the height 2 to 6, bits 0 and 1 clear (all
        // as measured: red follows PWV7's low byte, 0.72, blue its high
        // byte, 0.50, the height is PWV3's, 0.97, and bits 0 and 1 were 0
        // in every entry). Each colour is its band against the strongest
        // of the three, in eighths: a guess at the rule, not a fit.
        const float strongest = std::max({e.low, e.mid, e.high});
        const auto shade = [&](float band) {
            return strongest > 0.0f ? clampByte(7.0 * band / strongest, 7.0) : std::uint8_t{0};
        };
        const auto word = static_cast<std::uint16_t>((shade(e.low) << 13) | (shade(e.mid) << 10) |
                                                     (shade(e.high) << 7) | (height << 2));
        appendU16BE(colour, word);
        for (const std::uint8_t b : scrollBands(e)) {
            threeBand.push_back(static_cast<char>(b));
        }
    }

    // The 1200-column previews.
    const auto wide = domain::reduceWaveform(entries, WidePreviewColumns);
    std::string threeBandPreview, colourPreview;
    threeBandPreview.reserve(WidePreviewColumns * 3);
    colourPreview.reserve(WidePreviewColumns * 6);
    for (std::uint32_t j = 0; j < WidePreviewColumns; ++j) {
        const domain::WaveformEntry c = j < wide.size() ? wide[j] : domain::WaveformEntry{};
        const std::array<double, 3> level{c.low * ScrollBandGain[0] * ThreeBandPreviewGain[0],
                                          c.mid * ScrollBandGain[1] * ThreeBandPreviewGain[1],
                                          c.high * ScrollBandGain[2] * ThreeBandPreviewGain[2]};
        for (const double v : level) {
            threeBandPreview.push_back(static_cast<char>(clampByte(v, 127.0)));
        }
        // PWV4, six bytes a column. Measured only by correlation with
        // PWV6 over 200 tracks; the rule is a guess. Bytes 2 and 3 follow
        // PWV6's low (0.87, 0.90; gains about 2.7 and 2.5), 4 its mid
        // (0.43, gain 1.8), 5 its high (0.45, gain 1.3); byte 0 (0 to
        // 127, median 121) the overall level, byte 1 (0 to 255) falls as
        // the low rises (-0.54), so it is written as the share that is
        // not bass, like the whiteness.
        const double total = static_cast<double>(c.low) + c.mid + c.high;
        colourPreview.push_back(static_cast<char>(clampByte(127.0 * c.overall, 127.0)));
        colourPreview.push_back(static_cast<char>(total > 0.0 ? clampByte(255.0 * (c.mid + c.high) / total, 255.0) : 0));
        colourPreview.push_back(static_cast<char>(clampByte(2.7 * level[0], 127.0)));
        colourPreview.push_back(static_cast<char>(clampByte(2.5 * level[0], 127.0)));
        colourPreview.push_back(static_cast<char>(clampByte(1.8 * level[1], 127.0)));
        colourPreview.push_back(static_cast<char>(clampByte(1.3 * level[2], 127.0)));
    }

    // .EXT
    std::vector<std::string> extSections{path, section("PWV3", scrollHeader(1, scrollCount, 0), mono), hotLegacy,
                                         memoryLegacy, hotCues, memoryCues};
    if (!beats.empty()) {
        extSections.push_back(extendedBeatGridSection(beats));
    }
    extSections.push_back(section("PWV5", scrollHeader(2, scrollCount, 0x0305), colour));
    {
        std::string fields;
        appendU32BE(fields, 6);
        appendU32BE(fields, WidePreviewColumns);
        appendU32BE(fields, 0);
        extSections.push_back(section("PWV4", fields, colourPreview));
    }
    files.ext = fileOf(extSections);

    // .2EX
    std::string pwv6Fields;
    appendU32BE(pwv6Fields, 3);
    appendU32BE(pwv6Fields, WidePreviewColumns);
    std::string pwvcFields;
    appendU16BE(pwvcFields, 0);
    std::string pwvcBody;
    for (const std::uint16_t v : WaveformColourValues) {
        appendU16BE(pwvcBody, v);
    }
    files.twoEx = fileOf({path, section("PWV7", scrollHeader(3, scrollCount, 0), threeBand),
                          section("PWV6", pwv6Fields, threeBandPreview), section("PWVC", pwvcFields, pwvcBody)});

    files.threeBandPreview = std::move(threeBandPreview);
    files.colourPreview = std::move(colourPreview);
    files.scrollEntries = scrollCount;
    return files;
}

std::string newAnlzPathFor(const std::string &trackPath, const std::unordered_set<std::string> &analyzePathsInUse)
{
    constexpr std::uint32_t Directories = 0x80;
    constexpr std::uint32_t Names = 0x30000;
    // FNV-1a over the path's bytes: any spread will do, this only picks
    // where the search starts.
    std::uint32_t hash = 0x811C9DC5u;
    for (const unsigned char c : trackPath) {
        hash = (hash ^ c) * 0x01000193u;
    }
    // A folder is taken when any file in it is named, whatever its number
    // (rekordbox puts a second track's analysis in ANLZ0001) and however
    // the hex digits are cased.
    std::unordered_set<std::string> takenFolders;
    for (const auto &used : analyzePathsInUse) {
        std::string folder = used.substr(0, used.rfind('/') == std::string::npos ? 0 : used.rfind('/'));
        std::transform(folder.begin(), folder.end(), folder.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        takenFolders.insert(std::move(folder));
    }
    const std::uint64_t slots = static_cast<std::uint64_t>(Directories) * Names;
    const std::uint64_t start = hash % slots;
    for (std::uint64_t k = 0; k < slots; ++k) {
        const std::uint64_t slot = (start + k) % slots;
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "/PIONEER/USBANLZ/P%03X/%08X", static_cast<unsigned>(slot % Directories),
                      static_cast<unsigned>(slot / Directories));
        const std::string folder(buffer);
        if (!takenFolders.contains(folder)) {
            return folder + "/ANLZ0000.DAT";
        }
    }
    throw std::runtime_error("every analysis folder is taken");
}

}  // namespace seabass::infrastructure::rekordbox
