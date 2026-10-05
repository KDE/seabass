// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// The planting, reading and comparing behind tools/onelibrary_cue_source_probe
// (issue #59): where a OneLibrary player takes its cues from. Kept apart
// from the tool's main() so tests/onelibrary_cue_source_probe_test.cpp can
// run all of it against a copy of a committed fixture, without a stick.
//
// Five tracks, each planted to answer one question on the deck:
//
//   T1  pad A at 0:45 in the OneLibrary cue table only; the analysis file
//       holds no cue at all. A pad means the player reads the table.
//   T2  the legacy PCOB hot list holds pad A at 0:20 only; the modern PCO2
//       list holds A 0:20, B 1:00, C 1:40. One pad means PCOB, three PCO2.
//   T3  a loop on pad A, 0:30 to 0:38, in the analysis file only (both
//       lists), no table row. A lit loop means it reads the file's loops.
//   T4  pad B at 1:15 written by Seabass's own Add Cue for a OneLibrary
//       row (AddCueChange through the save loop), so what users get.
//   T5  pad A at 0:50 in the table and at 0:55 in the analysis file (both
//       lists). Which time the pad jumps to says which source wins.
//
// Every write goes through the project's writers: OneLibraryCueWriter for
// the table (which writes the analysis file first; the file is then
// reshaped by RekordboxCueWriter), and, for T2's legacy list, the PCOB
// codec through AnlzFile's checked atomic write and the writer's own
// read-back check.

#include <QJsonObject>

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace seabass::omnis59
{

// One entry of any of the five cue lists, reduced to what a player shows.
struct ListEntry
{
    uint32_t slot = 0;  // 1-8 hot cue (A-H), 0 in a memory list
    uint32_t timeMs = 0;
    bool isLoop = false;
    uint32_t loopEndMs = 0;

    bool operator==(const ListEntry &other) const = default;
};

// Every cue list one track's analysis file pair holds, decoded section by
// section. A list the file does not have is empty; `problems` says what
// could not be read (a missing .EXT, a damaged section).
struct AnalysisLists
{
    bool extPresent = false;
    bool datPresent = false;
    std::vector<ListEntry> pco2Hot;      // .EXT PCO2, hot list
    std::vector<ListEntry> pco2Memory;   // .EXT PCO2, memory list
    std::vector<ListEntry> extPcobHot;   // .EXT PCOB, hot cues 4-8
    std::vector<ListEntry> extPcobMemory;
    std::vector<ListEntry> datPcobHot;   // .DAT PCOB, hot cues 1-3
    std::vector<ListEntry> datPcobMemory;
    std::vector<std::string> problems;
};

AnalysisLists readAnalysisLists(const std::string &pioneerRoot, const std::string &analyzePath);

// One row of exportLibrary.db's cue table.
struct TableCue
{
    int64_t kind = 0;  // 0 memory, otherwise the hot cue slot
    int64_t inUsec = 0;
    int64_t outUsec = 0;
    int64_t isActiveLoop = 0;
    std::string comment;  // empty for NULL

    bool operator==(const TableCue &other) const = default;
};

// A track present in DeviceLibrary and OneLibrary, one row in each, both
// naming the same analysis file, which is on the stick.
struct Candidate
{
    std::string title;
    std::string artist;
    std::string filePath;      // absolute
    std::string contentPath;   // OneLibrary content.path, "/Contents/..."
    std::string contentId;     // OneLibrary content_id
    std::string deviceLibraryId;  // export.pdb track id
    std::string analysisFile;  // "/PIONEER/USBANLZ/.../ANLZ0000.DAT"
    double durationSeconds = 0.0;
};

// Candidates on the stick at `stickRoot` (its PIONEER folder is read).
// `requireAudio` also wants the audio file itself on the stick, which a
// real deck needs and a committed fixture does not have.
std::vector<Candidate> findCandidates(const std::string &stickRoot, bool requireAudio, std::ostream &log);

// Five of them: shortest distinctive titles first, each title unique in
// OneLibrary (so a search on the deck finds exactly one), long enough for
// T2's 1:40 pad. Fewer than five when the stick has fewer.
std::vector<Candidate> pickFive(const std::vector<Candidate> &candidates);

// The analysis-file and table state each Tn is planted to have.
struct Expected
{
    std::vector<ListEntry> pco2Hot;
    std::vector<ListEntry> extPcobHot;
    std::vector<ListEntry> datPcobHot;
    std::vector<TableCue> table;
};
Expected expectedFor(int t);  // t = 1..5

// Plants T1..T5 on `five` (in that order). Throws on the first write that
// fails; the stick then needs restoring from its backup.
void plant(const std::string &stickRoot, const std::vector<Candidate> &five, std::ostream &log);

// Reads every planted track back from scratch and says where it is not
// exactly as planted: every list of both analysis files (memory lists
// must be empty), and every table row. Empty when all five are exact.
std::vector<std::string> verifyPlants(const std::string &stickRoot, const std::vector<Candidate> &five);

// What the deck shows for each track, and what each observation means.
std::string card(const std::vector<Candidate> &five, const std::string &stickLabel);

// The stick's state as far as this test cares: the five tracks (decoded
// lists, table rows, their content rows), every table's row count and
// digest, the history tables row by row, and every file's size and
// SHA-256. Read-only: exportLibrary.db is read from a copy on this
// computer, never opened on the stick.
QJsonObject snapshot(const std::string &stickRoot, const std::vector<Candidate> &five);

// Compares the stick now against a snapshot() taken after prepare and
// writes the report. Returns the number of differences found.
int readback(const std::string &stickRoot, const QJsonObject &before, std::ostream &out);

}  // namespace seabass::omnis59
