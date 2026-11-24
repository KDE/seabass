// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/cue_offset_repair.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "domain/sync_planning.hpp"

namespace seabass::domain
{

namespace
{

constexpr int ValidTagShiftSamples = 2292;
constexpr int NoTagShiftSamples = 229;
// How far apart two cues may be and still be one cue here: past the
// largest difference this check knows (the player's 2292 samples on top
// of a LAME file's 51 ms) and well under any two cues a DJ sets apart.
constexpr double PairWithinMs = 120.0;

bool isRekordboxSide(const std::string &format)
{
    return format == "rekordbox" || format == "onelibrary";
}

bool isHotPoint(const CuePoint &cue)
{
    return cue.kind == CuePoint::Kind::Hot && !cue.isLoop;
}

// Which rekordbox cue each Engine cue is a copy of, one to one, within
// PairWithinMs. A hot cue only ever pairs with the hot cue on its own pad:
// a cue on another pad is another cue however near. The main cue (Engine's
// memory cue) and loops pair with the nearest unused rekordbox cue of the
// same shape (a memory cue with a memory cue, a loop with a loop, Engine
// putting rekordbox's memory loops on its loop slots), each used once.
std::vector<std::pair<size_t, size_t>> pairCues(const std::vector<CuePoint> &engine,
                                                const std::vector<CuePoint> &rekordbox)
{
    std::vector<std::pair<size_t, size_t>> pairs;
    std::vector<bool> engineUsed(engine.size(), false);
    std::vector<bool> rekordboxUsed(rekordbox.size(), false);
    for (size_t e = 0; e < engine.size(); ++e) {
        if (!isHotPoint(engine[e])) {
            continue;
        }
        for (size_t r = 0; r < rekordbox.size(); ++r) {
            if (!rekordboxUsed[r] && isHotPoint(rekordbox[r]) && rekordbox[r].hotCueNumber == engine[e].hotCueNumber
                && std::abs(rekordbox[r].positionMs - engine[e].positionMs) < PairWithinMs) {
                pairs.emplace_back(e, r);
                engineUsed[e] = true;
                rekordboxUsed[r] = true;
                break;
            }
        }
    }
    // The rest: every candidate pair, nearest first, each cue taken once.
    struct Candidate
    {
        double distance;
        size_t e;
        size_t r;
    };
    std::vector<Candidate> candidates;
    for (size_t e = 0; e < engine.size(); ++e) {
        if (engineUsed[e] || isHotPoint(engine[e])) {
            continue;
        }
        for (size_t r = 0; r < rekordbox.size(); ++r) {
            if (rekordboxUsed[r] || isHotPoint(rekordbox[r]) || rekordbox[r].isLoop != engine[e].isLoop) {
                continue;
            }
            if (!engine[e].isLoop && rekordbox[r].kind != CuePoint::Kind::Memory) {
                continue;
            }
            const double distance = std::abs(rekordbox[r].positionMs - engine[e].positionMs);
            if (distance < PairWithinMs) {
                candidates.push_back({distance, e, r});
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) { return a.distance < b.distance; });
    for (const Candidate &c : candidates) {
        if (!engineUsed[c.e] && !rekordboxUsed[c.r]) {
            pairs.emplace_back(c.e, c.r);
            engineUsed[c.e] = true;
            rekordboxUsed[c.r] = true;
        }
    }
    std::sort(pairs.begin(), pairs.end());
    return pairs;
}

std::string millis(double ms)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.0f ms", std::abs(ms));
    return text;
}

std::string dateOf(const std::string &when)
{
    return when.size() >= 10 ? when.substr(0, 10) : when;
}

}  // namespace

std::optional<int> playerFirstLoadShiftSamples(const Mp3LeadIn &leadIn)
{
    if (leadIn.vbri || !leadIn.tagFrameFirst || !leadIn.tagFrameHasCounts) {
        return std::nullopt;
    }
    if (!leadIn.infoFrame) {
        return NoTagShiftSamples;
    }
    switch (leadIn.tag) {
    case Mp3LeadIn::Tag::None:
        return NoTagShiftSamples;
    case Mp3LeadIn::Tag::Lavc:
        return ValidTagShiftSamples;
    case Mp3LeadIn::Tag::Lame:
        return leadIn.lameCrcValid ? ValidTagShiftSamples : NoTagShiftSamples;
    case Mp3LeadIn::Tag::LameL3:
    case Mp3LeadIn::Tag::Other:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<LoggedCueCopy> parseLoggedCueCopy(std::string_view line)
{
    // 2026-09-30T20:00:00  sync: copied cues (1 hot) from rekordbox track "T" (id=106) to engine track id=104
    // ... to onelibrary track "file.mp3"
    static constexpr std::string_view Marker = "  sync: copied cues (";
    const size_t marker = line.find(Marker);
    if (marker == std::string_view::npos) {
        return std::nullopt;
    }
    LoggedCueCopy copy;
    copy.when = std::string(line.substr(0, marker));
    std::string_view rest = line.substr(marker + Marker.size());
    static constexpr std::string_view From = ") from ";
    const size_t from = rest.find(From);
    if (from == std::string_view::npos) {
        return std::nullopt;
    }
    rest = rest.substr(from + From.size());
    static constexpr std::string_view Track = " track \"";
    const size_t sourceTrack = rest.find(Track);
    if (sourceTrack == std::string_view::npos) {
        return std::nullopt;
    }
    copy.sourceFormat = std::string(rest.substr(0, sourceTrack));
    rest = rest.substr(sourceTrack + Track.size());
    // The title may hold quotes of its own: the id after the last `" (id=`
    // that is followed by `) to ` ends it.
    static constexpr std::string_view IdOpen = "\" (id=";
    static constexpr std::string_view To = ") to ";
    const size_t to = rest.rfind(To);
    if (to == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t idOpen = rest.rfind(IdOpen, to);
    if (idOpen == std::string_view::npos) {
        return std::nullopt;
    }
    copy.sourceTitle = std::string(rest.substr(0, idOpen));
    copy.sourceId = std::string(rest.substr(idOpen + IdOpen.size(), to - idOpen - IdOpen.size()));
    rest = rest.substr(to + To.size());
    static constexpr std::string_view TrackId = " track id=";
    const size_t targetId = rest.find(TrackId);
    if (targetId != std::string_view::npos) {
        copy.targetFormat = std::string(rest.substr(0, targetId));
        copy.targetId = std::string(rest.substr(targetId + TrackId.size()));
    } else {
        const size_t targetTrack = rest.find(Track);
        if (targetTrack == std::string_view::npos || rest.size() < targetTrack + Track.size() + 1
            || rest.back() != '"') {
            return std::nullopt;
        }
        copy.targetFormat = std::string(rest.substr(0, targetTrack));
        copy.targetFilename =
            std::string(rest.substr(targetTrack + Track.size(), rest.size() - targetTrack - Track.size() - 1));
    }
    while (!copy.targetId.empty() && (copy.targetId.back() == '\r' || copy.targetId.back() == ' ')) {
        copy.targetId.pop_back();
    }
    if (copy.sourceFormat.empty() || copy.sourceId.empty() || copy.targetFormat.empty()
        || (copy.targetId.empty() && copy.targetFilename.empty())) {
        return std::nullopt;
    }
    return copy;
}

CueOffsetSide CueOffsetFinding::sideOf(size_t index, std::optional<CueOffsetSide> pick) const
{
    const CueOffsetSide side = cues.at(index).side;
    return side == CueOffsetSide::Unknown && pick ? *pick : side;
}

bool CueOffsetFinding::moves(CueOffsetSide side, std::optional<CueOffsetSide> pick) const
{
    for (size_t i = 0; i < cues.size(); ++i) {
        if (sideOf(i, pick) == side) {
            return true;
        }
    }
    return false;
}

bool CueOffsetFinding::hasKnown() const
{
    for (const CueOffset &offset : cues) {
        if (offset.side != CueOffsetSide::Unknown) {
            return true;
        }
    }
    return false;
}

bool CueOffsetFinding::hasUnknown() const
{
    for (const CueOffset &offset : cues) {
        if (offset.side == CueOffsetSide::Unknown) {
            return true;
        }
    }
    return false;
}

std::vector<CuePoint> CueOffsetFinding::cuesMoved(CueOffsetSide side, std::optional<CueOffsetSide> pick) const
{
    const bool onEngine = side == CueOffsetSide::Engine;
    std::vector<CuePoint> cuesOut = onEngine ? engine.cues : rekordbox.cues;
    for (size_t i = 0; i < cues.size(); ++i) {
        if (sideOf(i, pick) != side) {
            continue;
        }
        const CueOffset &offset = cues[i];
        const CuePoint &from = onEngine ? rekordbox.cues[offset.rekordboxIndex] : engine.cues[offset.engineIndex];
        CuePoint &cue = cuesOut[onEngine ? offset.engineIndex : offset.rekordboxIndex];
        cue.positionMs = from.positionMs;
        if (cue.isLoop) {
            cue.loopEndMs = from.loopEndMs;
        }
    }
    return cuesOut;
}

double CueOffsetFinding::engineStoredMs(size_t index) const
{
    return engine.cues[cues.at(index).engineIndex].positionMs - engine.cueTimelineShiftMs;
}

std::string CueOffsetFinding::reason() const
{
    const std::string rekordboxName = catalogDisplayName(rekordbox.format);
    int byPlayer = 0;
    int toEngine = 0;
    int toRekordbox = 0;
    int unknown = 0;
    for (const CueOffset &offset : cues) {
        if (offset.cause == CueOffsetCause::MovedByPlayer) {
            ++byPlayer;
        } else if (offset.side == CueOffsetSide::Engine) {
            ++toEngine;
        } else if (offset.side == CueOffsetSide::Rekordbox) {
            ++toRekordbox;
        } else {
            ++unknown;
        }
    }
    const auto count = [](int n) { return n == 1 ? std::string("1 cue") : std::to_string(n) + " cues"; };
    std::vector<std::string> parts;
    if (byPlayer > 0) {
        // Engine's sits the player's shift before rekordbox's; it belongs
        // `shiftMs` before it.
        const double playerMs = *playerShiftSamples * 1000.0 / sampleRate;
        const double off = shiftMs - playerMs;  // positive: Engine's is late
        parts.push_back("A Denon player moved " + count(byPlayer) + " on Engine when it first loaded this track: "
                        + millis(off) + (off > 0 ? " late" : " early") + " there; Engine's go back to where "
                        + rekordboxName + "'s say.");
    }
    const std::string when = loggedCopy ? " on " + dateOf(loggedCopy->when) : std::string();
    if (toEngine > 0) {
        parts.push_back(count(toEngine) + " copied from " + rekordboxName + " to Engine" + when
                        + " by a Seabass build that did not move cues for this MP3's start: Engine's are "
                        + millis(shiftMs) + " late and move.");
    }
    if (toRekordbox > 0) {
        parts.push_back(count(toRekordbox) + " copied from Engine to " + rekordboxName + when
                        + " by a Seabass build that did not move cues for this MP3's start: " + rekordboxName
                        + "'s are " + millis(shiftMs) + " early and move.");
    }
    if (unknown > 0) {
        // Only ever without a logged copy of this pair: one names the side
        // for every cue at the same number.
        parts.push_back(count(unknown) + " hold the same number on both sides, but this file's sound sits "
                        + millis(shiftMs) + " apart on " + rekordboxName
                        + " and Engine. No copy of this track is logged on the stick: pick the side that is right.");
    }
    std::string text;
    for (const std::string &part : parts) {
        text += (text.empty() ? "" : " ") + part;
    }
    return text;
}

std::optional<CueOffsetFinding> findCueOffsets(const Track &rekordboxSide, const Track &engine,
                                               const std::optional<Mp3LeadIn> &leadIn,
                                               const std::vector<LoggedCueCopy> &log)
{
    if (!leadIn || leadIn->sampleRate <= 0 || engine.format != "engine" || !isRekordboxSide(rekordboxSide.format)
        || engine.engineCuesOnRekordboxTimeline || !engine.streamingSource.empty()) {
        return std::nullopt;
    }
    const double shiftMs = engineToRekordboxOffsetMs(*leadIn);
    // The reader put Engine's cues on rekordbox's timeline by this same
    // shift, and the writer takes it off again; with the correction off,
    // or another number read, nothing here can be judged.
    if (std::abs(engine.cueTimelineShiftMs - shiftMs) > 1e-6 || std::abs(engine.engineCueWriteShiftMs - shiftMs) > 1e-6) {
        return std::nullopt;
    }
    CueOffsetFinding finding;
    finding.rekordbox = rekordboxSide;
    finding.engine = engine;
    finding.shiftMs = shiftMs;
    finding.sampleRate = leadIn->sampleRate;
    finding.playerShiftSamples = playerFirstLoadShiftSamples(*leadIn);
    const double rate = leadIn->sampleRate;
    const std::optional<double> playerMs = finding.playerShiftSamples
        ? std::optional<double>(*finding.playerShiftSamples * 1000.0 / rate)
        : std::nullopt;
    const bool playerShiftDiffers = playerMs && std::abs(*playerMs - shiftMs) > CueOffsetSameWithinMs;

    // The last logged copy between these two rows, either way.
    std::optional<LoggedCueCopy> logged;
    std::optional<CueOffsetSide> loggedSide;
    for (const LoggedCueCopy &copy : log) {
        const bool toEngine = copy.sourceFormat == rekordboxSide.format && copy.sourceId == rekordboxSide.sourceId
            && copy.sourceTitle == rekordboxSide.title && copy.targetFormat == "engine"
            && copy.targetId == engine.sourceId;
        const bool toRekordbox = copy.sourceFormat == "engine" && copy.sourceId == engine.sourceId
            && copy.sourceTitle == engine.title && copy.targetFormat == rekordboxSide.format
            && (rekordboxSide.format == "onelibrary" ? copy.targetFilename == rekordboxSide.filename
                                                     : copy.targetId == rekordboxSide.sourceId);
        if (toEngine || toRekordbox) {
            logged = copy;
            loggedSide = toEngine ? CueOffsetSide::Engine : CueOffsetSide::Rekordbox;
        }
    }

    for (const auto &[e, r] : pairCues(engine.cues, rekordboxSide.cues)) {
        const CuePoint &engineCue = engine.cues[e];
        const CuePoint &rekordboxCue = rekordboxSide.cues[r];
        const double storedMs = engineCue.positionMs - engine.cueTimelineShiftMs;
        const double difference = rekordboxCue.positionMs - storedMs;
        if (engineCue.isLoop) {
            const double endDifference = rekordboxCue.loopEndMs - (engineCue.loopEndMs - engine.cueTimelineShiftMs);
            if (std::abs(endDifference - difference) > CueOffsetSameWithinMs) {
                continue;  // a loop of another length: another loop
            }
        }
        if (std::abs(difference - shiftMs) <= CueOffsetSameWithinMs) {
            continue;  // where it belongs
        }
        CueOffset offset;
        offset.rekordboxIndex = r;
        offset.engineIndex = e;
        if (std::abs(shiftMs) > CueOffsetSameWithinMs && std::abs(difference) <= CueOffsetSameWithinMs) {
            offset.cause = CueOffsetCause::CopiedUnmoved;
            offset.side = loggedSide.value_or(CueOffsetSide::Unknown);
        } else if (playerShiftDiffers && (engineCue.kind == CuePoint::Kind::Hot || engineCue.isLoop)
                   && std::abs(difference * rate / 1000.0 - *finding.playerShiftSamples)
                       <= PlayerShiftWithinSamples + 1e-6) {
            offset.cause = CueOffsetCause::MovedByPlayer;
            offset.side = CueOffsetSide::Engine;
        } else {
            continue;  // another difference: a different cue, Sync's to show
        }
        finding.cues.push_back(offset);
    }
    if (finding.cues.empty()) {
        return std::nullopt;
    }
    finding.sideToMove = finding.cues.front().side;
    bool anyLogged = false;
    for (const CueOffset &offset : finding.cues) {
        if (offset.side != finding.sideToMove) {
            finding.sideToMove = CueOffsetSide::Unknown;
        }
        anyLogged = anyLogged || (offset.cause == CueOffsetCause::CopiedUnmoved && offset.side != CueOffsetSide::Unknown);
    }
    if (anyLogged) {
        finding.loggedCopy = logged;
    }
    return finding;
}

}  // namespace seabass::domain
