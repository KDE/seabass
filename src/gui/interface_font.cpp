// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/interface_font.hpp"

#include <QFontDatabase>
#include <QFontInfo>
#include <QRawFont>
#include <QString>
#include <QStringList>

namespace seabass::gui
{

namespace
{

// Real families, in the order they are worth having, one per platform
// this project ships on plus the two a Linux container is likely to
// carry. Every one of them is a name a font database can resolve, which
// is the entire point: the thing being replaced is a category.
const QStringList &fallbackFamilies()
{
    static const QStringList families{
        QStringLiteral("Segoe UI"),        // Windows
        QStringLiteral("Helvetica Neue"),  // macOS
        QStringLiteral("Noto Sans"),       // most Linux desktops
        QStringLiteral("DejaVu Sans"),     // what a bare container has
        QStringLiteral("Liberation Sans"),
        QStringLiteral("Arial"),
    };
    return families;
}

// Whether the face a font actually resolves to can draw the interface.
// Not whether the family is LISTED, which was the first rule here and
// was wrong: macOS's own interface font is ".AppleSystemUIFont", which
// QFontDatabase::families() deliberately does not list, so that rule
// threw away a perfectly good platform font and replaced it with
// Helvetica Neue. Different metrics, different elision, and a page test
// that had been green went red -- caught on macOS before this landed.
//
// Coverage is the property that actually matters, and it is what fails
// in the case this exists for: the three-glyph symbol subset resolves,
// is a real face, and cannot draw a letter.
bool drawsLatin(const QFont &font)
{
    const QRawFont face = QRawFont::fromFont(font);
    if (!face.isValid()) {
        return false;
    }
    for (const QChar c : QStringLiteral("EngineOS")) {
        if (!face.supportsCharacter(c)) {
            return false;
        }
    }
    return true;
}

// The family a request actually resolves to, right now. A GENERIC name
// -- "Sans Serif" is what Windows hands back -- is resolved afresh every
// time it is used, and what it resolves to changes the moment this app
// registers a font of its own. Theme loads a three-glyph symbol subset
// through a FontLoader as soon as the QML engine starts, which is after
// this runs, and Windows then resolved "Sans Serif" to that subset:
// every label asking for the app font and being handed a face with no
// Latin coverage at all.
//
// Measured rather than reasoned. On Windows the app font read "Sans
// Serif" from the very first test in the process, before any failing
// one, and which face it resolved to differed BETWEEN PROCESS LAUNCHES
// of the same binary: "Segoe UI Variable" in one run, the symbol subset
// in the next two, same commit and same environment. A name that can
// mean two things is not a choice, so the concrete family is pinned
// here, while the answer is still the right one.
QFont pinnedToConcreteFamily(QFont font)
{
    // QRawFont first, QFontInfo second. QFontInfo returns an EMPTY family
    // for every font at this point in startup under the offscreen
    // platform on Windows -- measured there, including for a plain
    // QFont("Arial"): family "", pixelSize -1, exactMatch false. It does
    // no matching that early, so the guard below silently kept the
    // ambiguous name and the pin did nothing on the one platform it was
    // written for. QRawFont::fromFont() does load a face in exactly that
    // environment, which is how drawsLatin() above works at all.
    const QRawFont face = QRawFont::fromFont(font);
    const QString fromFace = face.isValid() ? face.familyName() : QString();
    if (!fromFace.isEmpty()) {
        font.setFamily(fromFace);
        return font;
    }
    const QString fromInfo = QFontInfo(font).family();
    if (!fromInfo.isEmpty()) {
        font.setFamily(fromInfo);
    }
    return font;
}

}  // namespace

QFont interfaceFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    if (drawsLatin(font)) {
        return pinnedToConcreteFamily(font);
    }

    for (const QString &candidate : fallbackFamilies()) {
        QFont trial = font;
        trial.setFamily(candidate);
        if (drawsLatin(trial)) {
            return pinnedToConcreteFamily(trial);
        }
    }

    // Nothing here can draw a letter: keep what the platform said rather
    // than inventing a name. Not pinned either -- pinning a family that
    // cannot draw would only make the wrong answer permanent. A font database in that state is a problem
    // this function cannot fix, and pretending otherwise would hide it.
    return font;
}

}  // namespace seabass::gui
