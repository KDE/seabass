#pragma once

#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// One memory cue sitting at the very start of a track -- in practice
// almost always an accidental/junk cue rather than a deliberate marker:
// unlike a hot cue (which some DJs do use at 0:00 on purpose as an
// explicit "track start" pad), a memory cue there serves no real
// navigational purpose (the track already starts at 0:00) and most
// often comes from a stray click during analysis or an import artifact.
//
// "At the start" means positionMs < 1000 (displays as "0:00" in every
// mm:ss field this app has, all of which floor to whole seconds -- see
// e.g. PlayerBar.qml's formatTime()), not literally positionMs == 0.
// Real Engine data confirmed this matters: Engine's own auto-generated
// main_cue routinely lands a few hundred ms into the track (its analysis
// picks the first detected beat/transient, not sample 0 exactly) --
// real examples seen include 286ms, 339ms, 539ms, and 750ms, none of
// which the old exact-zero check caught, so genuinely stray cues a user
// could see displayed as "0:00" were silently never offered for
// cleanup.

struct JunkCueIssue
{
    Track track;    // the track carrying the cue
    CuePoint cue;   // the specific memory cue at position 0
};

// Pure, no filesystem/database access -- callers already have a fresh
// track list from the same scan that also feeds
// LibraryConsistencyChecker, this just looks at cues directly rather
// than file existence.
// The one definition of "junk": a memory cue inside the first second,
// almost always a stray press while the track sat at the start. Shared
// by the finder and the remover so the two cannot disagree.
bool isJunkMemoryCue(const CuePoint &cue);

class JunkCueFinder
{
public:
    static std::vector<JunkCueIssue> find(const std::vector<Track> &tracks);
};

}  // namespace seabass::domain
