// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Every page that hosts an edit session must act on the low-space
// question's Cancel. QML cannot tell whether a signal is connected, and
// the failure is silent -- the dialog closes and nothing else happens,
// which is exactly how it went unnoticed on all nine pages -- so this
// reads the page sources instead.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "../src/infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;

int main()
{
    const fs::path qmlDir = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "src" / "gui" / "qml";
    int checked = 0;
    bool ok = true;
    for (const auto &entry : fs::recursive_directory_iterator(qmlDir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".qml"
            || entry.path().filename() == "EditSessionHost.qml") {
            continue;
        }
        std::ifstream in(entry.path());
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        if (text.find("EditSessionHost {") == std::string::npos) {
            continue;
        }
        ++checked;
        if (text.find("onBackupLocationDeclined") == std::string::npos) {
            std::cerr << seabass::pathToUtf8(entry.path().filename())
                      << ": hosts an edit session but ignores backupLocationDeclined. Cancel on the "
                         "low-space question would close the dialog and change nothing\n";
            ok = false;
        }
    }
    // A scan that found nothing proves nothing.
    assert(checked > 0 && "no page uses EditSessionHost: the scan is looking in the wrong place");
    if (!ok) {
        return 1;
    }
    std::cout << "edit_session_decline_wiring_test: " << checked << " pages act on Cancel\n";
    return 0;
}
