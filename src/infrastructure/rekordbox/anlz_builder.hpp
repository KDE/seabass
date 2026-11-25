// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "domain/beat_grid.hpp"
#include "domain/waveform_from_audio.hpp"

namespace seabass::infrastructure::rekordbox
{

// A track's three analysis files (.DAT, .EXT, .2EX) built from nothing,
// for a track Seabass puts on a stick itself: what a player needs to show
// a beat grid and waveforms, and the empty cue lists it expects to find.
// A first version, to be replaced behind this interface.
//
// The layout is rekordbox 7's, measured on its own exports (300 files of
// a stick, 5198 of a collection, and the testshark_t73 fixture):
//
//   file header  PMAI, len_header 28, len_file, then 00000001 00010000
//                00010000 00000000 (all but 20 of 5498 files; those 20
//                have 00000000 first)
//   .DAT  PPTH PVBR PQTZ PWAV PWV2 PCOB(hot) PCOB(memory)   every file
//   .EXT  PPTH PWV3 PCOB PCOB PCO2(hot) PCO2(memory) PQT2 PWV5 PWV4
//   .2EX  PPTH PWV7 PWV6 PWVC
//
// What is left out, and why:
//   - PSSI (song structure, the phrases) from the .EXT: it is masked with
//     a key and holds an analysis Seabass does not make; 12 of 1734
//     collection tracks have the same .EXT without it.
//   - PVB2 from the .EXT: unknown, on 22 of 1834 tracks.
//   - PVDI from the .2EX: unknown, 289 of 1732 collection tracks have it,
//     the other 1443 do not.
//   - PQT2 when there is no grid: the two measured tracks without beats
//     have an empty PQTZ and no PQT2.
//
// Every number written, and where it comes from, is in the .cpp.
struct NewAnlzInput
{
    // As PPTH spells it on a stick, e.g. "/Contents/Artist/Album/file.wav"
    // (UTF-8 here, UTF-16BE in the file).
    std::string trackPath;
    // In order, on the timeline of `waveform`. May be empty: no grid.
    // beatInBar 0 continues the count of the beat before (1 for a first
    // beat).
    std::vector<domain::Beat> beats;
    domain::AudioWaveform waveform;
    // Used only when `waveform` has no entries: then the scrolling
    // waveforms are this long and flat.
    double durationMs = 0.0;
    int sampleRateHz = 0;
    // PVBR. For an MP3, rekordbox stores the number of samples the
    // decoder makes, which is the frames after the Info/Xing frame times
    // 1152 (all 1312 MP3s measured), in the last of its 401 values, and
    // for a VBR file a seek table in the 400 before it (byte offsets at
    // each 1/400 of the track; 1 file of 1397 measured). For every other
    // format all 401 are 0. Leave both empty for anything but an MP3.
    std::uint32_t mp3SampleCount = 0;
    std::vector<std::uint32_t> mp3SeekOffsets;  // empty, or exactly 400
};

struct NewAnlzFiles
{
    // The complete files.
    std::string dat;
    std::string ext;
    std::string twoEx;
    // PWV6's body, as readWaveformSections() gives it (1200 x 3 bytes).
    std::string threeBandPreview;
    // PWV4's body (1200 x 6 bytes).
    std::string colourPreview;
    // Entries in PWV3, PWV5 and PWV7: 150 a second.
    std::uint32_t scrollEntries = 0;
};

// Throws std::invalid_argument on an input that cannot make a valid
// file: a path that is empty, not absolute, not UTF-8 or with a NUL in
// it, beats out of order or before 0, two beats closer than the PQTZ
// tempo field can say (above 655.35 BPM), a seek table that is not 400
// long, no length at all (no waveform entries and no positive
// durationMs), or a length of over a day.
//
// Not checked: that the beats end where the track does. A grid moved onto
// another timeline can reach past the last sample by a few milliseconds;
// the caller cuts it to the track if it wants that.
NewAnlzFiles buildAnlzFiles(const NewAnlzInput &input);

// Where a new track's files go: "/PIONEER/USBANLZ/Pxxx/xxxxxxxx/ANLZ0000.DAT",
// as export.pdb's analyze_path spells it.
//
// rekordbox derives the two folder names from the track somehow, but not
// from its path by any hash tried (CRC-32, CRC-32C, Adler-32, FNV-1 and
// 1a, djb2, sdbm, Java's string hash, MD5, SHA-1, over the path in UTF-8,
// UTF-16LE and UTF-16BE, with and without the leading slash, a NUL, case
// folding or /PIONEER in front; any bit window and a few moduli; against
// 100 tracks of a real stick). So this picks a folder of the same shape
// with no file of `analyzePathsInUse` in it: P000 to P07F and 00000000 to
// 0002FFFF, the ranges every real stick measured stays within. Pass the
// analyze paths of export.pdb (and exportLibrary.db, where there is one)
// and of every ANLZ file found under USBANLZ, spelled the same way, so a
// folder left behind without a row is not reused either. The start is a
// hash of the track path, so one path asks for the same folder every
// time, and the search walks on from there until it finds one free.
std::string newAnlzPathFor(const std::string &trackPath, const std::unordered_set<std::string> &analyzePathsInUse);

}  // namespace seabass::infrastructure::rekordbox
