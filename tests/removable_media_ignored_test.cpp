// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// With SEABASS_IGNORE_REMOVABLE_MEDIA set, the media factory's locator
// finds nothing, whatever is plugged in. The variable is what keeps the
// test suites off the real sticks (see media_factory.cpp), so the guard
// itself gets a test: an environment variable nobody reads is exactly the
// kind of protection that stops working without anyone noticing.

#include <cassert>
#include <cstdlib>
#include <iostream>

#include "infrastructure/media/media_factory.hpp"

namespace
{
void setVariable(const char *value)  // nullptr unsets
{
#if defined(_WIN32)
    _putenv_s("SEABASS_IGNORE_REMOVABLE_MEDIA", value ? value : "");
#else
    if (value) {
        setenv("SEABASS_IGNORE_REMOVABLE_MEDIA", value, 1);
    } else {
        unsetenv("SEABASS_IGNORE_REMOVABLE_MEDIA");
    }
#endif
}
}  // namespace

int main()
{
    setVariable("1");
    assert(seabass::infrastructure::media::removableMediaIgnored());
    assert(seabass::infrastructure::media::createRemovableMediaLocator()->detect().empty()
           && "ignored: the locator finds nothing");

    setVariable("0");
    assert(!seabass::infrastructure::media::removableMediaIgnored() && "\"0\" means not ignored");
    setVariable("");
    assert(!seabass::infrastructure::media::removableMediaIgnored() && "empty means not ignored");
    setVariable(nullptr);
    assert(!seabass::infrastructure::media::removableMediaIgnored() && "unset means not ignored");

    std::cout << "All removable_media_ignored tests passed.\n";
    return 0;
}
