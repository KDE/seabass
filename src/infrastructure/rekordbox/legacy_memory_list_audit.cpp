// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/legacy_memory_list_audit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>

#include "application/path_key.hpp"
#include "domain/local_restore.hpp"
#include "domain/track.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace fs = std::filesystem;

namespace
{

// A .DAT the RX2 wrote on its way down: its beat grid and both waveform
// previews are the empty sections (24 and 20 bytes, header only), where
// any analysis rekordbox exported has beats and pixels in all three.
bool isSkeleton(const std::string &datPath)
{
    try {
        const AnlzFile file = AnlzFile::readRaw(datPath);
        bool grid = false;
        bool wave = false;
        bool wave2 = false;
        for (const auto &section : file.sections) {
            switch (section.fourcc) {
            case 0x5051545A:  // "PQTZ"
                grid = section.rawBytes.size() == 24;
                break;
            case 0x50574156:  // "PWAV"
                wave = section.rawBytes.size() == 20;
                break;
            case 0x50575632:  // "PWV2"
                wave2 = section.rawBytes.size() == 20;
                break;
            default:
                break;
            }
        }
        return grid && wave && wave2;
    } catch (const std::exception &) {
        return false;
    }
}

bool isDatAnalysisName(const std::string &name)
{
    // ANLZ followed by digits and .DAT, as rekordbox and the players
    // spell it. Case kept strict: that is how both write it.
    if (name.size() < 9 || name.compare(0, 4, "ANLZ") != 0 || name.compare(name.size() - 4, 4, ".DAT") != 0) {
        return false;
    }
    return std::all_of(name.begin() + 4, name.end() - 4, [](unsigned char c) { return std::isdigit(c) != 0; });
}

// The PCO2 entry that is the same cue as `cue`, for its colour.
const ListedCue *partnerOf(const ListedCue &cue, const std::vector<ListedCue> &in, bool byPad)
{
    const ListedCue *best = nullptr;
    for (const auto &other : in) {
        if ((byPad && other.pad != cue.pad) || !sameListedCue(cue, other)) {
            continue;
        }
        if (!best || std::abs(double(other.timeMs) - double(cue.timeMs))
                         < std::abs(double(best->timeMs) - double(cue.timeMs))) {
            best = &other;
        }
    }
    return best;
}

std::string colorText(const std::optional<std::tuple<uint8_t, uint8_t, uint8_t>> &color)
{
    if (!color) {
        return {};
    }
    char text[8];
    std::snprintf(text, sizeof text, "#%02X%02X%02X", std::get<0>(*color), std::get<1>(*color),
                  std::get<2>(*color));
    return text;
}

domain::CuePoint cuePoint(const ListedCue &listedCue, bool hot)
{
    domain::CuePoint cue;
    cue.kind = hot ? domain::CuePoint::Kind::Hot : domain::CuePoint::Kind::Memory;
    cue.hotCueNumber = hot ? static_cast<int>(listedCue.pad) : 0;
    cue.positionMs = listedCue.timeMs;
    cue.isLoop = listedCue.isLoop;
    cue.loopEndMs = listedCue.isLoop ? listedCue.loopEndMs : 0;
    cue.color = colorText(listedCue.color);
    return cue;
}

// The cues both generations of list are rewritten from. Player: the
// legacy lists, each cue taking the colour of the PCO2 entry that is the
// same cue (RekordboxCueWriter then carries that entry's bytes over,
// comment and all, when the position is the same to the millisecond).
// Seabass: what Seabass shows, the PCO2 lists with the legacy cues they
// lack.
std::vector<domain::CuePoint> cuesToKeep(const TrackCueLists &lists, KeepCueList keep)
{
    std::vector<domain::CuePoint> cues;
    auto add = [&cues](std::vector<ListedCue> from, bool hot, const std::vector<ListedCue> *colours) {
        if (hot) {
            std::stable_sort(from.begin(), from.end(), [](const ListedCue &a, const ListedCue &b) {
                return a.pad < b.pad;
            });
        }
        for (ListedCue cue : from) {
            if (colours) {
                if (const ListedCue *partner = partnerOf(cue, *colours, hot)) {
                    cue.color = partner->color;
                }
            }
            cues.push_back(cuePoint(cue, hot));
        }
    };
    if (keep == KeepCueList::Player) {
        add(lists.legacyHot, true, &lists.modernHot);
        add(lists.legacyMemory, false, &lists.modernMemory);
    } else {
        add(seabassView(lists.modernHot, lists.legacyHot, true), true, nullptr);
        add(seabassView(lists.modernMemory, lists.legacyMemory, false), false, nullptr);
    }
    return cues;
}

TrackCueLists readListsOf(const std::string &datPath, const std::string &extPath)
{
    return cueListsOf(readCueSections(datPath), readCueSections(extPath), datPath, extPath);
}

// The file as written, read back with the writer's own check
// (analysisFileReadBackProblem). Puts `before` back and throws when it
// fails, as RekordboxCueWriter does for its own writes.
void readBackOrRestore(const std::string &path, const std::string &intended, const std::string &before)
{
    const std::optional<std::string> problem = analysisFileReadBackProblem(path, intended);
    if (!problem) {
        return;
    }
    const bool restored = writeFileDurablyAtomic(path, before);
    throw std::runtime_error(path + " failed its check after writing (" + *problem + "); "
                             + (restored ? "it was put back as it was"
                                         : "it could NOT be put back and needs restoring from the backup"));
}

}  // namespace

TrackCueLists readTrackCueLists(const std::string &pioneerRoot, const std::string &analyzePath)
{
    return readListsOf(datAnlzPath(pioneerRoot, analyzePath), extAnlzPath(pioneerRoot, analyzePath));
}

LegacyMemoryListFinding examineTrackAnalysis(const std::string &pioneerRoot, const std::string &paddedAnalyzePath,
                                             const std::function<bool(const std::string &)> &named)
{
    // export.pdb keeps its strings in fixed-length fields padded with
    // spaces (NULs in some), and the padding is no part of the name: the
    // same trim normalizedPathKey() makes, which the #59 test needs for a
    // row naming "ANLZ0000.DAT   ". Left on, the files were "missing" and
    // the debris rule cut the row's directory out of the padded string.
    std::string analyzePath = paddedAnalyzePath;
    while (!analyzePath.empty()
           && (analyzePath.back() == ' ' || analyzePath.back() == '\t' || analyzePath.back() == '\0')) {
        analyzePath.pop_back();
    }
    LegacyMemoryListFinding finding;
    finding.pioneerRoot = pioneerRoot;
    finding.analyzePath = analyzePath;
    const std::string dat = datAnlzPath(pioneerRoot, analyzePath);
    const std::string ext = extAnlzPath(pioneerRoot, analyzePath);
    const fs::path datFs = pathFromUtf8(dat);
    std::error_code ec;
    if (fs::is_regular_file(datFs, ec)) {
        finding.datPath = dat;
    }
    // The .DAT's memory list first, on its own: an RX2 hangs on it
    // whatever state the .EXT is in.
    std::vector<AnlzRawSection> datSections;
    bool datRead = false;
    if (finding.datPath.empty()) {
        finding.unreadable = dat + " is missing";
    } else {
        try {
            datSections = readCueSections(dat);
            datRead = true;
        } catch (const std::exception &e) {
            finding.unreadable = e.what();
        }
    }
    if (datRead) {
        for (const auto &section : datSections) {
            if (isMemoryPcob(section)) {
                finding.shape = auditLegacyMemoryList(section.rawBytes);
                break;
            }
        }
    }
    // Then the two generations compared, which needs the .EXT too.
    if (datRead) {
        std::vector<AnlzRawSection> extSections;
        if (!fs::is_regular_file(pathFromUtf8(ext), ec)) {
            finding.unreadable = ext + " is missing";
        } else {
            try {
                extSections = readCueSections(ext);
                finding.examined = true;
            } catch (const std::exception &e) {
                finding.unreadable = e.what();
            }
        }
        // The comparison the readers make of the same bytes when they
        // read a track's cues (compareCueSections), so the stick's
        // summary and this check count the same files.
        if (finding.examined) {
            CueListsCompared compared = compareCueSections(datSections, extSections, dat, ext);
            finding.disagreement = std::move(compared.disagreement);
            finding.listsMalformed = std::move(compared.listsMalformed);
        }
    }

    // What else answers to an analysis name in the track's directory,
    // and whether it is anybody's. rekordbox puts a second track's
    // analysis in the same directory as ANLZ0001.* (ten of them in the
    // committed fixture, each named from its own row), so the name
    // alone says nothing: debris is what no row names, has no .EXT, and
    // holds no analysis.
    const fs::path dir = datFs.parent_path();
    const std::string own = pathToUtf8(datFs.filename());
    const std::string rowDir = analyzePath.substr(0, analyzePath.size() - own.size());
    if (fs::is_directory(dir, ec)) {
        for (const auto &entry : fs::directory_iterator(dir, ec)) {
            const std::string name = pathToUtf8(entry.path().filename());
            if (name == own || !isDatAnalysisName(name) || !entry.is_regular_file(ec)) {
                continue;
            }
            if (named(rowDir + name)) {
                continue;
            }
            const fs::path extFile = entry.path().parent_path() / (name.substr(0, name.size() - 4) + ".EXT");
            if (fs::exists(extFile, ec)) {
                continue;
            }
            if (!isSkeleton(pathToUtf8(entry.path()))) {
                continue;
            }
            finding.debris.push_back(pathToUtf8(entry.path()));
        }
        std::sort(finding.debris.begin(), finding.debris.end());
    }
    return finding;
}

std::optional<LegacyMemoryListFinding> auditTrackAnalysis(const std::string &pioneerRoot,
                                                          const std::string &analyzePath,
                                                          const std::function<bool(const std::string &)> &named)
{
    LegacyMemoryListFinding finding = examineTrackAnalysis(pioneerRoot, analyzePath, named);
    if (!finding.anything()) {
        return std::nullopt;
    }
    return finding;
}

void CueListTally::add(const LegacyMemoryListFinding &finding)
{
    if (finding.examined) {
        ++examined;
    } else {
        ++unreadable;
    }
    if (finding.shape.headerStale || finding.shape.unlinked) {
        ++legacyHeader;
    }
    if (finding.shape.zeroSlots) {
        ++playerRewritten;
    }
    if (finding.disagreement && finding.disagreement->any()) {
        ++disagree;
    }
    strayFiles += finding.debris.size();
}

CueListTally &CueListTally::operator+=(const CueListTally &other)
{
    examined += other.examined;
    unreadable += other.unreadable;
    legacyHeader += other.legacyHeader;
    playerRewritten += other.playerRewritten;
    disagree += other.disagree;
    strayFiles += other.strayFiles;
    return *this;
}

CueListScan scanCueLists(const std::string &pioneerRoot, const std::vector<std::string> &analyzePaths,
                         const std::function<bool(const std::string &)> &named, const std::function<void()> &each)
{
    CueListScan scan;
    std::set<std::string> seen;
    std::set<std::string> seenDebris;
    for (const auto &analyzePath : analyzePaths) {
        if (each) {
            each();
        }
        if (analyzePath.empty() || !seen.insert(application::normalizedPathKey(analyzePath)).second) {
            continue;
        }
        LegacyMemoryListFinding finding = examineTrackAnalysis(pioneerRoot, analyzePath, named);
        // Two tracks analysed into one directory would each report the
        // same stray file; it is one file, reported and removed once.
        auto &debris = finding.debris;
        debris.erase(std::remove_if(debris.begin(), debris.end(),
                                    [&seenDebris](const std::string &file) {
                                        return !seenDebris.insert(application::normalizedPathKey(file)).second;
                                    }),
                     debris.end());
        scan.tally.add(finding);
        if (finding.anything() || !finding.unreadable.empty()) {
            scan.findings.push_back(std::move(finding));
        }
    }
    return scan;
}

std::string repairTrackAnalysis(const LegacyMemoryListFinding &finding, KeepCueList keep)
{
    std::string account;
    auto say = [&account](const std::string &what) { account += (account.empty() ? "" : "; ") + what; };
    if (finding.shape.repairable()) {
        AnlzFile file = AnlzFile::readRaw(finding.datPath);
        const std::string before = file.toBytes();
        bool replaced = false;
        for (auto &section : file.sections) {
            if (!isMemoryPcob(section)) {
                continue;
            }
            // Audited again from the bytes on disk now, not from the
            // scan's: a save in between (a sync of this track rewrites
            // the list) leaves nothing to do, and that is not an error.
            const LegacyMemoryListShape now = auditLegacyMemoryList(section.rawBytes);
            if (now.repairable()) {
                section.rawBytes = repairLegacyMemoryList(section.rawBytes);
                replaced = true;
            }
            break;
        }
        if (replaced) {
            file.writeRaw(finding.datPath);
            readBackOrRestore(finding.datPath, file.toBytes(), before);
            say("rebuilt the memory list of " + finding.datPath + " (" + std::to_string(finding.shape.entries)
                + " entries kept)");
        } else {
            say("memory list of " + finding.datPath + " was already in shape");
        }
    }
    if (finding.listsFixable()) {
        // From the files as they are now, the memory list just repaired
        // included, so the player's own entries are what the writer
        // carries over.
        const TrackCueLists now = readTrackCueLists(finding.pioneerRoot, finding.analyzePath);
        const auto disagreement = compareCueLists(now);
        if (!disagreement) {
            say("cue lists of " + finding.analyzePath + " already agree");
        } else if (!disagreement->unrepairable.empty()) {
            say("cue lists of " + finding.analyzePath + " left alone: " + disagreement->unrepairable);
        } else {
            const std::vector<domain::CuePoint> cues = cuesToKeep(now, keep);
            RekordboxCueWriter writer(finding.pioneerRoot);
            writer.writeCuesToAnalysisFile(finding.analyzePath, cues, RekordboxCueWriter::Rewrite::OnlyIfChanged);
            if (compareCueLists(readTrackCueLists(finding.pioneerRoot, finding.analyzePath))) {
                throw std::runtime_error("the cue lists of " + finding.analyzePath
                                         + " still disagree after writing them from one list");
            }
            say("rewrote the cue lists of " + finding.analyzePath + " from "
                + (keep == KeepCueList::Player ? "the player's" : "Seabass's") + " list ("
                + std::to_string(cues.size()) + " cues)");
        }
    }
    for (const auto &debris : finding.debris) {
        std::string failure;
        if (!removeEntry(pathFromUtf8(debris), failure)) {
            throw std::runtime_error("could not remove " + debris + ": " + failure);
        }
        say("removed " + debris);
    }
    return account;
}

}  // namespace seabass::infrastructure::rekordbox
