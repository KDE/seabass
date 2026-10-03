// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace seabass::infrastructure::rekordbox
{

// The legacy memory cue list (the .DAT file's memory PCOB, see
// anlz_legacy_cue_codec.hpp) in a shape a player chokes on (#33, #55).
//
// From 5282555e to 6e0f1c09 RekordboxCueWriter wrote the list with a
// header that says "empty" (0xFFFFFFFF) while it holds entries, and left
// two or more entries unlinked. An XDJ-RX2 (firmware 1.43) reads such a
// list as empty, drops the entries on its next memory save, and leaves a
// list sized for one more entry than it holds, the extra slot zero. Its
// memory-loop save after that hangs the player, which then has to be
// powered off and leaves a skeleton ANLZ0001.DAT behind. The writer fix
// reaches a track only when Seabass writes it again; this finds and
// repairs the rest.
struct LegacyMemoryListShape
{
    int entries = 0;
    // The header's last-entry field is not entries - 1 (0xFFFFFFFF when
    // the list is empty).
    bool headerStale = false;
    // Entries not chained by index: entry i must name i - 1 and i + 1,
    // 0xFFFF at either end.
    bool unlinked = false;
    // len_tag exceeds 24 + 56 * entries and the excess is all zero: the
    // player's rewrite of a stale list.
    bool zeroSlots = false;
    // Nonempty: not a list this can read, so not one it repairs either.
    std::string malformed;

    bool damaged() const { return headerStale || unlinked || zeroSlots; }
    bool repairable() const { return damaged() && malformed.empty(); }
};

// Looks at one memory PCOB section. A section whose type is not the
// memory list is reported as malformed.
LegacyMemoryListShape auditLegacyMemoryList(const std::string &pcobSectionBytes);

// The same list re-encoded with every entry kept byte for byte, the
// header and links set from the entries and the zero slots dropped.
// Throws std::runtime_error when the shape is malformed or not damaged.
std::string repairLegacyMemoryList(const std::string &pcobSectionBytes);

// One cue as one of the two generations of list holds it, for comparing
// them (#60). The legacy PCOB lists carry no colour; `color` is the
// modern PCO2 entry's, when it has one.
struct ListedCue
{
    uint32_t pad = 0;  // 1-8 for a hot cue, 0 for a memory cue
    uint32_t timeMs = 0;
    bool isLoop = false;
    uint32_t loopEndMs = 0;
    std::optional<std::tuple<uint8_t, uint8_t, uint8_t>> color;
};

// A track's four cue lists: the legacy PCOB lists of both files taken
// together (hot cues 1-3 and the memory cues in the .DAT, 4-8 in the
// .EXT), which is what an XDJ-RX2 and a CDJ-3000X show, and the modern
// PCO2 lists in the .EXT, which Seabass reads first (#33, #60).
struct TrackCueLists
{
    std::vector<ListedCue> legacyHot;
    std::vector<ListedCue> legacyMemory;
    std::vector<ListedCue> modernHot;
    std::vector<ListedCue> modernMemory;
    // Memory cues in the .EXT's legacy list, where neither rekordbox nor
    // a player puts any (all 3988 surveyed files) and the writer never
    // writes: counted in legacyMemory, and a reason to leave the track
    // alone, since no rewrite would bring that list in step.
    size_t extLegacyMemory = 0;
};

// Where the two generations disagree. From 2026-09-02 (the first cue
// writer) to 5282555e (2026-09-18) RekordboxCueWriter wrote PCO2 alone,
// so a track written then can show one set of pads on a player and
// another in Seabass.
//
// "The same cue" is decided as the reader decides it when it merges the
// two (kaitai_rekordbox_reader.cpp, appendLegacyCues): a hot cue by its
// pad, a memory cue by position, positions within
// LocalRestorePlanner::PositionToleranceMs. rekordbox itself stores one
// cue in the two lists a millisecond, or 52 ms, apart, and a check that
// compared exactly would offer to rewrite rekordbox's own files over
// that. A cue on one side only, or a pad more than the tolerance away,
// or a loop on one side and a cue on the other, is a disagreement.
// Cues are paired in order of position, so the pairing found is the one
// with the smallest gaps.
struct CueListDisagreement
{
    bool hot = false;
    bool memory = false;
    TrackCueLists lists;
    // What Seabass shows for the track: the modern lists, plus every
    // legacy cue they do not already hold (a hot cue on a pad PCO2 does
    // not use, a memory cue no PCO2 one is the same as). That is how the
    // reader merges the two, and what "keep what Seabass wrote" keeps, so
    // a cue only the legacy list has is not lost by choosing Seabass.
    std::vector<ListedCue> seabassHot;
    std::vector<ListedCue> seabassMemory;
    // The player and Seabass show the same cues; only the modern list
    // lacks some the legacy one has, which a reader of PCO2 alone (a
    // newer player, rekordbox) would not show.
    bool viewsAgree = false;
    // Nonempty: a list holds a cue the writer cannot put in both
    // generations (a hot cue outside pads 1-8), so it is left alone.
    std::string unrepairable;

    bool any() const { return hot || memory; }
};

// Which list wins when a track's two disagree. A choice the user makes
// per stick: Player keeps what the hardware has been showing (the
// legacy lists) and rebuilds PCO2 from it, colours carried over from
// PCO2 entries of the same cue; Seabass keeps what Seabass shows (see
// CueListDisagreement::seabassHot) and rebuilds the legacy lists from
// it. Either way both lists are written from one value per cue, so a
// cue whose two entries sat a few milliseconds apart ends up at the
// chosen side's position, and a PCO2 comment survives only where the
// PCO2 entry is kept to the millisecond (RekordboxCueWriter encodes no
// comment of its own).
enum class KeepCueList { Player, Seabass };

// Both files' lists. Throws std::runtime_error when either file is
// missing or cannot be read, or a list in either cannot be decoded; a
// memory PCOB in the shapes auditLegacyMemoryList() repairs is read
// leniently, its real entries only.
TrackCueLists readTrackCueLists(const std::string &pioneerRoot, const std::string &analyzePath);

// Nothing when the lists agree.
std::optional<CueListDisagreement> compareCueLists(const TrackCueLists &lists);

// One track's analysis directory, looked at.
struct LegacyMemoryListFinding
{
    // The stick's PIONEER directory and the row's analyze_path, so the
    // repair can go through RekordboxCueWriter.
    std::string pioneerRoot;
    std::string analyzePath;
    // The .DAT export.pdb names for the track; empty when the file is
    // missing, in which case only debris can be reported.
    std::string datPath;
    // Whether both files were opened and their lists read. When not,
    // `unreadable` says why (a file missing, or not an analysis file).
    // An unreadable track is counted, never passed as clean.
    bool examined = false;
    std::string unreadable;
    LegacyMemoryListShape shape;
    // The legacy and modern lists disagree (#60).
    std::optional<CueListDisagreement> disagreement;
    // A hot list or a PCO2 list that does not decode: not compared, so
    // not repaired either.
    std::string listsMalformed;
    // What the RX2 leaves behind when it hangs: an ANLZ*.DAT beside the
    // track's that no row names, with no .EXT of its own number, holding
    // an empty beat grid and waveform. Reported, and removed by the
    // repair, since nothing refers to it. (A second full set in the
    // directory is rekordbox's own: it numbers a second track's analysis
    // ANLZ0001 and names it from that track's row.)
    std::vector<std::string> debris;

    bool memoryListFinding() const { return shape.damaged() || !shape.malformed.empty() || !debris.empty(); }
    bool listsFinding() const { return (disagreement && disagreement->any()) || !listsMalformed.empty(); }
    bool anything() const { return memoryListFinding() || listsFinding(); }
    // Whether the repair would change anything.
    bool memoryListFixable() const { return shape.repairable() || !debris.empty(); }
    bool listsFixable() const
    {
        return disagreement && disagreement->any() && disagreement->unrepairable.empty() && listsMalformed.empty()
               && shape.malformed.empty();
    }
    bool fixable() const { return memoryListFixable() || listsFixable(); }
};

// The track's .DAT memory list and the files next to it, and the two
// generations of cue list compared. The memory list is audited from the
// .DAT alone, so a missing or damaged .EXT still leaves a list an RX2
// hangs on reported (and the track counted unreadable, not examined). `analyzePath` is the row's
// analyze_path as AnlzPathIndex gives it, and `named` answers whether
// any catalog row names a path spelled the same way (AnlzPathIndex::
// names, and OneLibrary's rows). Always returns, examined or not.
LegacyMemoryListFinding examineTrackAnalysis(const std::string &pioneerRoot, const std::string &analyzePath,
                                             const std::function<bool(const std::string &)> &named);

// The same, with nothing returned when the lists are healthy and agree
// and the directory holds no debris. Throws nothing a track can cause:
// an unreadable track is a finding of examineTrackAnalysis(), and is
// returned here only when its directory holds debris.
std::optional<LegacyMemoryListFinding> auditTrackAnalysis(const std::string &pioneerRoot,
                                                          const std::string &analyzePath,
                                                          const std::function<bool(const std::string &)> &named);

// What a scan of many tracks found, counted. Every count is of analysis
// files (a .DAT and .EXT pair), each once however many rows name it.
// `examined` is the honest one: a scan that opened nothing says 0 here,
// and the page says "nothing was checked", never "all is well".
struct CueListTally
{
    size_t examined = 0;
    size_t unreadable = 0;
    size_t legacyHeader = 0;      // header or links wrong (pre-6e0f1c09)
    size_t playerRewritten = 0;   // the RX2's zero slot
    size_t disagree = 0;          // legacy and modern lists differ
    size_t strayFiles = 0;        // debris files, not tracks

    void add(const LegacyMemoryListFinding &finding);
    CueListTally &operator+=(const CueListTally &other);
};

// Examines each analysis file once (rows naming the same file are one
// file) and counts. `findings` holds those with anything to report, and
// those that could not be read (so a page can name them), in the order
// first named. A stray file is reported once, with the first
// track in its directory, however many tracks share the directory. `each` is called once per path given, for a
// progress bar and a cancellation check; it may throw to stop.
struct CueListScan
{
    std::vector<LegacyMemoryListFinding> findings;
    CueListTally tally;
};
CueListScan scanCueLists(const std::string &pioneerRoot, const std::vector<std::string> &analyzePaths,
                         const std::function<bool(const std::string &)> &named,
                         const std::function<void()> &each = {});

// Repairs the track: the .DAT's memory list in place (through AnlzFile,
// with its staleness check, then read back), then, when the two
// generations disagree, every list rewritten from the side `keep` names
// through RekordboxCueWriter (which reads both files back), and last
// the debris removed. Each part is audited again from the files on
// disk first; one that a save since has already put right is left
// alone. Returns a one-line account. Throws on any failure, before or
// after a write, and when the lists still disagree afterwards; callers
// back up the .DAT, the .EXT and the debris first.
std::string repairTrackAnalysis(const LegacyMemoryListFinding &finding, KeepCueList keep = KeepCueList::Player);

}  // namespace seabass::infrastructure::rekordbox
