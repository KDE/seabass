#include "domain/junk_cue.hpp"

namespace seabass::domain
{

bool isJunkMemoryCue(const CuePoint &cue)
{
    return cue.kind == CuePoint::Kind::Memory && cue.positionMs >= 0.0 && cue.positionMs < 1000.0;
}

std::vector<JunkCueIssue> JunkCueFinder::find(const std::vector<Track> &tracks)
{
    std::vector<JunkCueIssue> issues;
    for (const auto &track : tracks) {
        for (const auto &cue : track.cues) {
            if (isJunkMemoryCue(cue)) {
                issues.push_back(JunkCueIssue{track, cue});
            }
        }
    }
    return issues;
}

}  // namespace seabass::domain
