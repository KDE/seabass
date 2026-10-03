// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "infrastructure/rekordbox/cue_list_check.hpp"

namespace seabass::infrastructure::rekordbox
{

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
