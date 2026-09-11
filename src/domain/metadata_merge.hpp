#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// One rule for deciding whose copy of an authored field survives, used
// in both directions: reading a stick into the metadata store, and
// putting the store back on a stick.
//
// It replaces the pair of radio buttons those two pages used to ask
// ("take the stick's version" / "keep what is stored"), and the reason
// is that the question was unanswerable. A run covers 1500 tracks; the
// stick is newer for some of them and the store is newer for others,
// and one answer applied to all of them is wrong for half. Worse, the
// only honest way to choose was to know which copy held more work,
// which is exactly what the machine can see and the person cannot.
//
// Three steps, in order:
//
//   1. Content beats emptiness. A field one side does not have is not a
//      conflict, it is a blank, and a blank is always filled. Nothing
//      here ever replaces a value with nothing.
//   2. More cues beats fewer. A cue set is a body of work rather than a
//      single value: trading four cues for one is a loss however recent
//      the one is. Only decides when the two sets actually differ.
//   3. Otherwise the later modification time wins.
//
// Ties keep what is already there. Two sides that disagree with nothing
// to separate them are a coin toss, and a coin toss that rewrites data
// on every run is worse than one that leaves it alone.
//
// Modification times are seconds since the epoch, 0 meaning "unknown".
// An unknown time never beats a known one, so a reading we could not
// take cannot silently overwrite work we can date.

// True when `incoming` should replace `existing` for a field that is
// either present or absent: a rating, a comment, a play count.
//
// `valuesDiffer` is the caller's own comparison, because what counts as
// a difference is field-specific and this must not guess: a rating is an
// int, a comment is text, and neither wants the other's rule.
bool takeIncomingField(bool incomingHasContent, bool existingHasContent, bool valuesDiffer,
                        std::int64_t incomingModifiedAt, std::int64_t existingModifiedAt);

// The same three steps for a cue set, with step 2 in play. Returns false
// when the two sets are equal: there is nothing to write and rewriting
// them would only churn the file.
bool takeIncomingCues(const std::vector<CuePoint> &incoming, const std::vector<CuePoint> &existing,
                       std::int64_t incomingModifiedAt, std::int64_t existingModifiedAt);

// Convenience wrappers so call sites read as the rule rather than as
// three bools in the right order.
bool takeIncomingRating(const std::optional<int> &incoming, const std::optional<int> &existing,
                         std::int64_t incomingModifiedAt, std::int64_t existingModifiedAt);
bool takeIncomingComment(const std::string &incoming, const std::string &existing,
                          std::int64_t incomingModifiedAt, std::int64_t existingModifiedAt);
bool takeIncomingPlayCount(const std::optional<int> &incoming, const std::optional<int> &existing,
                            std::int64_t incomingModifiedAt, std::int64_t existingModifiedAt);

// The rule as a sentence, for the help popups on both pages. Kept next
// to the code that implements it so the two cannot drift: a help text
// describing a policy the program no longer follows is worse than none.
std::string mergeRuleExplanation();

}  // namespace seabass::domain
