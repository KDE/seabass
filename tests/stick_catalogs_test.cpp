// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#include "gui/stick_catalogs.hpp"
#include "infrastructure/paths/utf8_path.hpp"

using seabass::gui::catalogPathForFormat;
namespace fs = std::filesystem;

// catalogPathForFormat() answers in the forward-slash form, because every
// caller turns it into a QString path, and a QString path has forward
// slashes on every platform (gui/qt_path.hpp). The native form put
// "E:\Music\PIONEER" into a RestoreMetadataChange on Windows while the
// page held "E:/Music/PIONEER".
//
// Only Windows has a second separator, so only there can the native form
// differ and this test go red; on Linux and macOS it pins the answer.
int main()
{
    // A stick mounted below a folder, the case where the native form puts
    // a backslash between the stick root and the catalog directory. On
    // Windows fs::path takes the drive letter; elsewhere it is one more
    // directory name, and the expectation is the same string either way.
#ifdef _WIN32
    const std::string stick = "E:/Music/STICK";
#else
    const std::string stick = "/media/dj/STICK";
#endif
    for (const std::string &opened : {stick + "/PIONEER", stick + "/Engine Library"}) {
        for (const char *format : {"rekordbox", "onelibrary", "engine"}) {
            const std::string path = catalogPathForFormat(opened, format);
            if (path.find('\\') != std::string::npos) {
                std::cerr << "a backslash in " << path << " (opened at " << opened << ", format " << format << ")\n";
                return 1;
            }
            const std::string expected =
                std::string(format) == "engine" ? stick + "/Engine Library" : stick + "/PIONEER";
            if (path != expected) {
                std::cerr << "expected " << expected << ", got " << path << '\n';
                return 1;
            }
            // And it is its own generic form: nothing a QString round trip
            // through pathToQString would respell.
            assert(path == seabass::pathToGenericUtf8(seabass::pathFromUtf8(path)));
        }
    }
    return 0;
}
