#pragma once

#include <string_view>

namespace seabass::application
{

// True when `path` is `root` itself, or something inside it.
//
// A string comparison rather than a filesystem one on purpose: the
// question this answers is "was this page opened against a stick that is
// no longer in the list", and by the time it is asked the stick is gone,
// so there is nothing left to stat. Both sides are paths the app already
// holds -- a catalog directory a page was handed, and a mount point the
// media locator reported -- and neither is user input.
//
// The separator is required, which is the whole reason this is not a
// bare starts_with: /media/RV2 must not answer for /media/RV22, and a
// stick whose label is a prefix of another's label is the ordinary case
// on a desk with several of them ("MAIN" and "MAIN-BACKUP").
inline bool pathIsUnder(std::string_view path, std::string_view root)
{
    if (root.empty() || path.empty()) {
        return false;
    }
    if (path == root) {
        return true;
    }
    // A root that already ends in a separator (a drive root such as
    // "D:\\", or "/") needs no second one adding.
    const bool rootEndsWithSeparator = root.back() == '/' || root.back() == '\\';
    if (path.size() <= root.size()) {
        return false;
    }
    if (path.compare(0, root.size(), root) != 0) {
        return false;
    }
    if (rootEndsWithSeparator) {
        return true;
    }
    const char next = path[root.size()];
    return next == '/' || next == '\\';
}

}  // namespace seabass::application
