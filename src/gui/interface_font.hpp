// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFont>

namespace seabass::gui
{

// The font this app draws its interface in, chosen rather than inherited.
//
// Nothing in the QML sets a family on ordinary labels, so they take the
// application default. Where the platform theme supplies a real family
// that is right and this returns it unchanged. Where it does not, the
// default is a GENERIC name -- "Sans Serif" is what Windows reported --
// and a generic name the font database cannot resolve falls back to an
// application font. The only one this app registers is the three-glyph
// symbol subset behind Theme.symbolFamily, which has no Latin coverage
// whatsoever, so every label in the app renders empty. That is what the
// Windows rig measured: names asking for "Sans Serif" and being handed
// "Seabass Symbols".
//
// QFontDatabase::systemFont(GeneralFont) answers correctly under a real
// platform plugin and does NOT answer under the offscreen one, which is
// what the whole test suite runs on, so asking the platform is necessary
// and not sufficient. The rule here is the one thing that holds in both:
// the face the font resolves to has to be able to draw a letter.
//
// Deliberately NOT "the family must be one the database lists", which
// this tried first: macOS's own interface font is ".AppleSystemUIFont",
// which families() does not list, so that rule discarded the right font
// and changed every metric in the app.
QFont interfaceFont();

}  // namespace seabass::gui
