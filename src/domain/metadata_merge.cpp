#include "domain/metadata_merge.hpp"

#include "domain/track_matching.hpp"

namespace seabass::domain
{

namespace
{

// Step 3, shared by both entry points. An unknown time (0) loses to a
// known one, and a tie keeps what is already there.
bool incomingIsLater(std::int64_t incomingModifiedAt, std::int64_t existingModifiedAt)
{
    return incomingModifiedAt > existingModifiedAt;
}

}  // namespace

bool takeIncomingField(bool incomingHasContent, bool existingHasContent, bool valuesDiffer,
                        std::int64_t incomingModifiedAt, std::int64_t existingModifiedAt)
{
    // Never replace a value with nothing. This is the one step that is
    // not a preference but a guarantee: a catalog that failed to read a
    // comment reports an empty one, and a rule without this line would
    // let a failed read erase the comment it failed to read.
    if (!incomingHasContent) {
        return false;
    }
    if (!existingHasContent) {
        return true;
    }
    if (!valuesDiffer) {
        return false;
    }
    return incomingIsLater(incomingModifiedAt, existingModifiedAt);
}

bool takeIncomingCues(const std::vector<CuePoint> &incoming, const std::vector<CuePoint> &existing,
                       std::int64_t incomingModifiedAt, std::int64_t existingModifiedAt)
{
    if (incoming.empty()) {
        return false;
    }
    if (existing.empty()) {
        return true;
    }
    // cueSetsEqual, not operator==: it ignores ordering and allows the
    // sub-second drift a cross-format conversion introduces, so two
    // spellings of one cue set do not read as a conflict and get
    // rewritten on every single run.
    if (cueSetsEqual(incoming, existing)) {
        return false;
    }
    if (incoming.size() != existing.size()) {
        return incoming.size() > existing.size();
    }
    return incomingIsLater(incomingModifiedAt, existingModifiedAt);
}

bool takeIncomingRating(const std::optional<int> &incoming, const std::optional<int> &existing,
                         std::int64_t incomingModifiedAt, std::int64_t existingModifiedAt)
{
    // has_value(), not a non-zero test: a track rated zero stars is
    // rated, and the store keeps that apart from unrated on purpose.
    return takeIncomingField(incoming.has_value(), existing.has_value(),
                              incoming.has_value() && existing.has_value() && *incoming != *existing,
                              incomingModifiedAt, existingModifiedAt);
}

bool takeIncomingComment(const std::string &incoming, const std::string &existing,
                          std::int64_t incomingModifiedAt, std::int64_t existingModifiedAt)
{
    return takeIncomingField(!incoming.empty(), !existing.empty(), incoming != existing, incomingModifiedAt,
                              existingModifiedAt);
}

bool takeIncomingPlayCount(const std::optional<int> &incoming, const std::optional<int> &existing,
                            std::int64_t incomingModifiedAt, std::int64_t existingModifiedAt)
{
    return takeIncomingField(incoming.has_value(), existing.has_value(),
                              incoming.has_value() && existing.has_value() && *incoming != *existing,
                              incomingModifiedAt, existingModifiedAt);
}

std::string mergeRuleExplanation()
{
    return "## When both copies have something\n\n"
           "Seabass does not ask you to choose a side for a whole run, because the answer differs "
           "track by track. It applies one rule, in this order:\n\n"
           "- **A blank is always filled.** A field only one copy has is not a disagreement. Nothing "
           "here ever replaces a value with an empty one, so a catalog that failed to read a comment "
           "cannot erase it.\n"
           "- **More cues wins.** When two cue sets genuinely differ, the larger one is kept. A cue set "
           "is a body of work, and trading four cues for one is a loss however recent the one is.\n"
           "- **Otherwise the more recent edit wins**, comparing when the stick's catalogs were last "
           "written against when this entry was last stored.\n\n"
           "A tie leaves what is already there alone.";
}

}  // namespace seabass::domain
