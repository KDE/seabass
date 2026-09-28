// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// A stick-shaped copy of tests/fixtures/anonymized_library's rekordbox
// tree, for a test that writes to it.
//
// USBANLZ is hard-linked rather than copied, because on Windows the copy
// was most of the test suite's time. It holds ~6,000 small analysis
// files, and Defender scans every newly created file the first time it
// is read: measured on the Windows shakedown laptop, a fresh copy of the
// tree took 13-15 s to make and 107-117 s to read once, a second read
// 3.2 s, and a tree of hard links to the already-scanned files 3.2 s.
// mirror_failure_fails_the_change_test makes sixteen copies and ran into
// ctest's 900 s cap; seabass_qml_tests pays the same per stick copy.
//
// A link is safe here because nothing writes an analysis file in place:
// every writer (anlz_file.cpp, the save loop's rollback through
// copyFileDurablyAtomic) goes through writeFileDurablyAtomic, a temp file
// renamed over the target, which replaces the link and leaves the
// committed fixture alone. Everything else -- the rekordbox databases,
// which SQLite writes in place and which the mirror tests make
// read-only, and the settings files -- is really copied.
//
// Where a link cannot be made (the scratch folder on another volume than
// the source tree), it falls back to a copy, which is only slower.

#include <filesystem>
#include <system_error>

namespace seabass::testing
{

inline void copyPioneerFixture(const std::filesystem::path &from, const std::filesystem::path &to,
                               std::error_code &ec)
{
    namespace fs = std::filesystem;
    ec.clear();
    fs::create_directories(to, ec);
    if (ec) {
        return;
    }
    for (const fs::directory_entry &entry : fs::directory_iterator(from, ec)) {
        const fs::path target = to / entry.path().filename();
        if (entry.path().filename() == "USBANLZ") {
            std::error_code linkEc;
            fs::copy(entry.path(), target, fs::copy_options::recursive | fs::copy_options::create_hard_links,
                     linkEc);
            if (!linkEc) {
                continue;
            }
            fs::remove_all(target, linkEc);
        }
        fs::copy(entry.path(), target, fs::copy_options::recursive, ec);
        if (ec) {
            return;
        }
    }
}

}  // namespace seabass::testing
