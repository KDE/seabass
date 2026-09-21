// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The interface font has to be a font.
//
// Windows measured every label in the app asking for the generic family
// "Sans Serif" and being handed "Seabass Symbols" -- the three-glyph
// symbol subset this app loads for the catalog icons, which supports
// none of E, n, g, i, O or S. Every label rendered empty, and the only
// reason it was caught at all is that a test happened to be measuring
// where one glyph sat.
//
// Two things are checked here, and the second is the one that matters:
// the family is one the font database lists, and the face it resolves to
// can actually draw Latin letters. A family name that resolves to a
// symbol subset passes the first and fails the second.
//
// Deliberately run under the offscreen platform, because that is what
// the whole QML suite runs under and it is where asking the platform for
// its interface font stops working: QFontDatabase::systemFont() answers
// correctly under a real plugin and does not under this one.

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QRawFont>
#include <QString>
#include <QStringList>

#include <cassert>
#include <iostream>

#include "gui/interface_font.hpp"

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const QFont font = seabass::gui::interfaceFont();
    const QStringList families = QFontDatabase::families();
    std::cout << "interface font: \"" << font.family().toStdString() << "\", "
              << families.size() << " families available\n";

    assert(!font.family().isEmpty());
    assert(!families.isEmpty());
    // Deliberately NOT "the family is one families() lists". macOS's own
    // interface font is ".AppleSystemUIFont", which is hidden from that
    // list, and asserting on it was what made an earlier version of this
    // throw the platform's font away and change every metric in the app.
    std::cout << "case 1 (a family was chosen, and the database is not empty) OK\n";

    const QRawFont face = QRawFont::fromFont(font);
    assert(face.isValid());
    for (const QChar c : QStringLiteral("EngineOS")) {
        assert(face.supportsCharacter(c));
    }
    std::cout << "case 2 (the face it resolves to draws Latin letters) OK\n";

    // And the thing that went wrong: whatever this returns, it must not
    // be the symbol subset, whose whole coverage is three shapes.
    assert(font.family() != QStringLiteral("Seabass Symbols"));
    std::cout << "case 3 (it is not the bundled symbol subset) OK\n";

    std::cout << "all cases passed\n";
    return 0;
}
