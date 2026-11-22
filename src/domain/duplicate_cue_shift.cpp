// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/duplicate_cue_shift.hpp"

#include <algorithm>
#include <cmath>

namespace seabass::domain
{

bool groupCarriesCues(const DuplicateGroup &group)
{
    for (const auto &track : group.tracks) {
        if (!track.cues.empty()) {
            return true;
        }
        for (const auto &row : track.catalogRows) {
            if (!row.cues.empty()) {
                return true;
            }
        }
    }
    return false;
}

size_t cueShiftPairsToMeasure(const std::vector<DuplicateGroup> &groups, AudioAlignmentProbe *probe)
{
    size_t pairs = 0;
    for (const auto &group : groups) {
        if (group.tracks.size() < 2 || !groupCarriesCues(group)) {
            continue;
        }
        const std::string &first = group.tracks.front().filePath;
        for (size_t i = 1; i < group.tracks.size(); ++i) {
            const std::string &path = group.tracks[i].filePath;
            if (path.empty() || path == first || (probe != nullptr && probe->answersAtOnce(first, path))) {
                continue;
            }
            ++pairs;
        }
    }
    return pairs;
}

namespace
{

// Which copies are lined up with which, as they are found: each copy
// belongs to the set of its anchor, at a shift from it.
class Sets
{
public:
    explicit Sets(size_t copies) : m_anchor(copies), m_ms(copies, 0.0)
    {
        for (size_t i = 0; i < copies; ++i) {
            m_anchor[i] = i;
        }
    }

    bool together(size_t a, size_t b) const { return m_anchor[a] == m_anchor[b]; }

    // b's music is `ms` later than a's: b's whole set joins a's.
    void join(size_t a, size_t b, double ms)
    {
        if (together(a, b)) {
            return;
        }
        const size_t from = m_anchor[b];
        const double moved = m_ms[a] + ms - m_ms[b];
        for (size_t k = 0; k < m_anchor.size(); ++k) {
            if (m_anchor[k] == from) {
                m_anchor[k] = m_anchor[a];
                m_ms[k] += moved;
            }
        }
    }

    // Only copies lined up with at least one other copy have a shift: a
    // copy alone in its set is unknown.
    std::vector<std::optional<DuplicateGroup::CueShift>> shifts() const
    {
        std::vector<std::optional<DuplicateGroup::CueShift>> out(m_anchor.size());
        for (size_t i = 0; i < m_anchor.size(); ++i) {
            const bool alone = std::count(m_anchor.begin(), m_anchor.end(), m_anchor[i]) == 1;
            if (!alone) {
                out[i] = DuplicateGroup::CueShift{m_anchor[i], m_ms[i]};
            }
        }
        return out;
    }

private:
    std::vector<size_t> m_anchor;
    std::vector<double> m_ms;
};

void measureGroup(DuplicateGroup &group, AudioAlignmentProbe *probe, const CueTimelineOffset &offset,
                  const std::function<void(bool)> &measured)
{
    using Kind = AudioAlignmentProbe::Answer::Kind;
    const size_t n = group.tracks.size();
    auto path = [&](size_t i) -> const std::string & { return group.tracks[i].filePath; };
    Sets sets(n);

    // One file listed twice, and files with the same bytes: lined up on
    // every timeline, whatever either file's offset between them is.
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < i && !path(i).empty(); ++j) {
            if (path(j).empty() || sets.together(j, i)) {
                continue;
            }
            if (path(j) == path(i) || (probe != nullptr && probe->sameContent(path(j), path(i)))) {
                sets.join(j, i, 0.0);
            }
        }
    }

    // Everything else against a reference. The offsets are asked before
    // anything is decoded: a copy whose cue timeline cannot be known can
    // be lined up with nothing, however well its audio matches.
    std::vector<std::optional<double>> offsets(n);
    std::vector<bool> asked(n, false);
    auto offsetOf = [&](size_t i) {
        if (!asked[i]) {
            asked[i] = true;
            offsets[i] = offset ? offset(path(i)) : std::optional<double>(0.0);
        }
        return offsets[i];
    };
    std::vector<bool> unusable(n, false);
    for (size_t i = 0; i < n; ++i) {
        unusable[i] = path(i).empty();
    }
    if (probe == nullptr) {
        group.cueShifts = sets.shifts();
        return;
    }
    for (size_t reference = 0; reference < n; ++reference) {
        if (unusable[reference] || !offsetOf(reference)) {
            continue;
        }
        bool referenceFailed = false;
        for (size_t i = 0; i < n && !referenceFailed; ++i) {
            if (i == reference || unusable[i] || sets.together(reference, i) || !offsetOf(i)) {
                continue;
            }
            const bool costly = !probe->answersAtOnce(path(reference), path(i));
            const AudioAlignmentProbe::Answer answer = probe->align(path(reference), path(i));
            if (measured) {
                measured(costly);
            }
            if (answer.kind == Kind::LinedUp) {
                sets.join(reference, i, answer.shiftMs + *offsetOf(i) - *offsetOf(reference));
                continue;
            }
            if (answer.unreadableB) {
                unusable[i] = true;
            }
            if (answer.unreadableA) {
                // Nothing can be measured against this one: the next copy
                // is the reference, and asks again whatever this one did
                // not settle.
                unusable[reference] = true;
                referenceFailed = true;
            }
        }
        if (!referenceFailed) {
            break;
        }
    }
    group.cueShifts = sets.shifts();
}

}  // namespace

void measureCueShifts(std::vector<DuplicateGroup> &groups, AudioAlignmentProbe *probe,
                      const CueTimelineOffset &offset, const std::function<void(bool)> &measured)
{
    for (auto &group : groups) {
        group.cueShifts.clear();
        group.cueShiftsMeasured = false;
        if (group.tracks.size() < 2 || !groupCarriesCues(group)) {
            continue;
        }
        measureGroup(group, probe, offset, measured);
        group.cueShiftsMeasured = true;
    }
}

std::optional<double> cueShiftBetween(const DuplicateGroup &group, size_t from, size_t to)
{
    const size_t n = group.tracks.size();
    if (from >= n || to >= n) {
        return std::nullopt;
    }
    if (from == to) {
        return 0.0;
    }
    const std::string &pathFrom = group.tracks[from].filePath;
    if (!pathFrom.empty() && pathFrom == group.tracks[to].filePath) {
        return 0.0;
    }
    if (!group.cueShiftsMeasured || group.cueShifts.size() != n) {
        return std::nullopt;
    }
    const auto &a = group.cueShifts[from];
    const auto &b = group.cueShifts[to];
    if (!a || !b || a->anchor != b->anchor) {
        return std::nullopt;
    }
    const double shift = b->ms - a->ms;
    return std::abs(shift) < NegligibleCueShiftMs ? 0.0 : shift;
}

std::vector<CuePoint> shiftedCues(const std::vector<CuePoint> &cues, double shiftMs)
{
    if (std::abs(shiftMs) < NegligibleCueShiftMs) {
        return cues;
    }
    std::vector<CuePoint> moved = cues;
    for (auto &cue : moved) {
        cue.positionMs += shiftMs;
        if (cue.isLoop) {
            cue.loopEndMs += shiftMs;
        }
    }
    return moved;
}

MovedCues movedCues(const std::vector<CuePoint> &cues, double shiftMs, double targetSeconds)
{
    MovedCues result;
    if (std::abs(shiftMs) < NegligibleCueShiftMs) {
        result.cues = cues;
        return result;
    }
    const double lengthMs = targetSeconds > 0.0 ? targetSeconds * 1000.0 : 0.0;
    auto inside = [lengthMs](double ms) { return ms >= 0.0 && (lengthMs <= 0.0 || ms <= lengthMs); };
    for (CuePoint cue : cues) {
        cue.positionMs += shiftMs;
        if (cue.isLoop) {
            cue.loopEndMs += shiftMs;
        }
        // A cue moved off either end of the track marks a sound that copy
        // does not have. Written anyway it would be clamped or refused by
        // the player, so it is counted and said instead.
        if (!inside(cue.positionMs) || (cue.isLoop && !inside(cue.loopEndMs))) {
            ++result.leftOut;
            continue;
        }
        result.cues.push_back(cue);
    }
    return result;
}

}  // namespace seabass::domain
