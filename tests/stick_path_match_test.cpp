#include <cassert>
#include <iostream>

#include "application/stick_path_match.hpp"

using seabass::application::pathIsUnder;

int main()
{
    // The ordinary case: a page holds a catalog directory, the media
    // locator reports the mount point it sits under.
    assert(pathIsUnder("/media/RV2/PIONEER", "/media/RV2"));
    assert(pathIsUnder("/media/RV2/Engine Library/m.db", "/media/RV2"));
    // The mount point itself counts as being on itself.
    assert(pathIsUnder("/media/RV2", "/media/RV2"));

    // The case this exists for. A bare prefix test answers true here,
    // and a stick whose label is a prefix of another's is the ordinary
    // situation on a desk with several of them: pulling RV2 would leave
    // a page reading RV22 convinced its stick was still there, and
    // pulling RV22 would raise the warning on RV2's page instead.
    assert(!pathIsUnder("/media/RV22/PIONEER", "/media/RV2"));
    assert(!pathIsUnder("/media/RV2-BACKUP/PIONEER", "/media/RV2"));
    assert(!pathIsUnder("/media/MAIN-BACKUP", "/media/MAIN"));

    // A different stick entirely.
    assert(!pathIsUnder("/media/OTHER/PIONEER", "/media/RV2"));
    // A parent is not under its own child.
    assert(!pathIsUnder("/media", "/media/RV2"));

    // Windows, where both separators turn up and a drive root already
    // ends in one.
    assert(pathIsUnder("D:\\PIONEER\\rekordbox", "D:\\"));
    assert(pathIsUnder("D:\\PIONEER", "D:"));
    assert(!pathIsUnder("D:\\PIONEER", "E:"));
    assert(pathIsUnder("E:/PIONEER", "E:"));

    // Nothing on either side is a match rather than a crash. An empty
    // mount point is what the locator reports for a stick that is
    // present but not mounted, and that must not answer for every path
    // there is.
    assert(!pathIsUnder("/media/RV2/PIONEER", ""));
    assert(!pathIsUnder("", "/media/RV2"));
    assert(!pathIsUnder("", ""));

    std::cout << "stick_path_match_test passed\n";
    return 0;
}
