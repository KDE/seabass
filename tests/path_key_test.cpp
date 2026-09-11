// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// normalizedPathKey() is the comparison three destructive decisions rest
// on: whether a file on disk is referenced by any catalog
// (findUnreferencedFiles), whether a file queued for deletion is still
// needed (resolvePendingDeletions), and whether two catalog rows describe
// one file or two (collapseCatalogRows).
//
// The failure that matters is one-directional. If two spellings of ONE
// file produce different keys, a referenced file is reported as
// unreferenced and offered for deletion, and a file still in the catalog
// is deleted from the pending queue. Every case here is therefore a pair
// that must agree.
#include <cassert>
#include <iostream>
#include <string>

#include "application/path_key.hpp"

using seabass::application::normalizedPathKey;

namespace
{
void same(const std::string &a, const std::string &b, const char *what)
{
    if (normalizedPathKey(a) != normalizedPathKey(b)) {
        std::cerr << "FAILED: " << what << "\n  [" << normalizedPathKey(a) << "]\n  [" << normalizedPathKey(b) << "]\n";
        assert(false);
    }
}
}  // namespace

int main()
{
    // exFAT and NTFS are case-insensitive, so these are one file.
    same("/Contents/a.mp3", "/CONTENTS/A.MP3", "ASCII case");

    // A stick written on Windows read on Linux: std::filesystem only
    // treats '\' as a separator on Windows, so this cannot be left to
    // the platform.
    same("/Contents/a.mp3", "\\Contents\\a.mp3", "backslash separators");

    same("/Contents/./a.mp3", "/Contents/a.mp3", "dot component");
    same("/Contents//a.mp3", "/Contents/a.mp3", "doubled separator");
    same("/Contents/x/../a.mp3", "/Contents/a.mp3", "parent component");

    // THE ONE THAT BIT. export.pdb stores its strings in fixed-length
    // fields and space-pads them, so every path read out of a rekordbox
    // catalog arrives with trailing spaces, while the same path walked
    // off the filesystem does not. Comparing them unequal means every
    // audio file on the stick reads as unreferenced.
    same("/Contents/a.mp3", "/Contents/a.mp3   ", "export.pdb's trailing space padding");
    same("/Contents/a.mp3", "/Contents/a.mp3\t", "trailing tab");
    same("/Contents/a.mp3", std::string("/Contents/a.mp3\0\0", 17), "trailing NULs");

    // Padding must not eat a name that really ends in a space before the
    // extension, which is a different thing entirely.
    assert(normalizedPathKey("/Contents/a .mp3") != normalizedPathKey("/Contents/a.mp3"));

    // A trailing separator names the same thing as none.
    same("/Contents/a.mp3", "/Contents/a.mp3/", "trailing separator");

    // Empty stays empty rather than becoming "." -- callers use empty as
    // "no path known" and must not have it collide with a real one.
    assert(normalizedPathKey("").empty());
    assert(normalizedPathKey("   ").empty());
    assert(normalizedPathKey("/").empty() || normalizedPathKey("/") == "/");

    // --- case beyond ASCII ------------------------------------------
    //
    // exFAT and NTFS are case-insensitive over Unicode, not over ASCII,
    // and this function's whole reason for lowercasing is that those are
    // the filesystems it runs on. Folding only A-Z meant an accented
    // name spelled in a different case read as a different file -- and
    // the way that error runs is that a referenced file becomes
    // unreferenced, which is what Seabass offers to delete.
    //
    // Music libraries are full of these: Café, Sigur Rós, Björk, Dvořák,
    // Łódź, Ελλάδα, Кино.
    same("/Contents/café.mp3", "/Contents/CAFÉ.mp3", "Latin-1: é/É");
    same("/Contents/rós.mp3", "/Contents/RÓS.mp3", "Latin-1: ó/Ó");
    same("/Contents/björk.mp3", "/Contents/BJÖRK.mp3", "Latin-1: ö/Ö");
    same("/Contents/ñu.mp3", "/Contents/ÑU.mp3", "Latin-1: ñ/Ñ");
    same("/Contents/dvořák.mp3", "/Contents/DVOŘÁK.mp3", "Latin Extended-A: ř/Ř");
    same("/Contents/łódź.mp3", "/Contents/ŁÓDŹ.mp3", "Latin Extended-A: ł/Ł and ź/Ź");
    same("/Contents/žluť.mp3", "/Contents/ŽLUŤ.mp3", "Latin Extended-A: ž/Ž, ť/Ť");
    same("/Contents/šum.mp3", "/Contents/ŠUM.mp3", "Latin Extended-A: š/Š");
    same("/Contents/ελλάδα.mp3", "/Contents/ΕΛΛΆΔΑ.mp3", "Greek");
    same("/Contents/кино.mp3", "/Contents/КИНО.mp3", "Cyrillic");
    same("/Contents/ђак.mp3", "/Contents/ЂАК.mp3", "Cyrillic extended (U+0400 block)");

    // Boundaries. A wrong offset in any block would fold a character
    // that is not a letter onto one that is, silently merging two files
    // that are genuinely different.
    assert(normalizedPathKey("/×.mp3") == "/×.mp3");   // U+00D7 multiplication sign, not a letter
    assert(normalizedPathKey("/÷.mp3") == "/÷.mp3");   // U+00F7 division sign
    assert(normalizedPathKey("/ß.mp3") == "/ß.mp3");   // no single-character uppercase
    assert(normalizedPathKey("/ĸ.mp3") == "/ĸ.mp3");   // U+0138, unpaired
    assert(normalizedPathKey("/ŉ.mp3") == "/ŉ.mp3");   // U+0149, unpaired
    assert(normalizedPathKey("/ſ.mp3") == "/ſ.mp3");   // U+017F, unpaired
    // Two genuinely different letters must not collapse onto each other.
    assert(normalizedPathKey("/a.mp3") != normalizedPathKey("/á.mp3"));
    assert(normalizedPathKey("/o.mp3") != normalizedPathKey("/ø.mp3"));
    assert(normalizedPathKey("/c.mp3") != normalizedPathKey("/č.mp3"));

    // Already-lowercase input is unchanged, in every block.
    for (const char *lower : {"/café.mp3", "/dvořák.mp3", "/ελλάδα.mp3", "/кино.mp3"}) {
        assert(normalizedPathKey(lower) == std::string(lower));
    }

    // Bytes that are not valid UTF-8 pass through rather than being
    // mangled: an undecodable path is still a real path to a real file,
    // and rewriting it would make two spellings of one file differ --
    // exactly the failure this function exists to prevent.
    // Built byte by byte rather than with escapes in a literal: \x
    // consumes as many hex digits as it can, so "\xFEbad" is one
    // over-long escape rather than a byte followed by "bad".
    std::string invalid = "/Contents/";
    invalid.push_back(static_cast<char>(0xFF));
    invalid.push_back(static_cast<char>(0xFE));
    invalid += "bad.mp3";
    // The undecodable bytes survive untouched; the decodable rest still
    // lowercases around them.
    std::string invalidLowered = "/contents/";
    invalidLowered.push_back(static_cast<char>(0xFF));
    invalidLowered.push_back(static_cast<char>(0xFE));
    invalidLowered += "bad.mp3";
    assert(normalizedPathKey(invalid) == invalidLowered);
    same(invalid, invalidLowered, "undecodable bytes do not stop the rest folding");

    std::string truncated = "/contents/caf";
    truncated.push_back(static_cast<char>(0xC3));  // lead byte with nothing after it
    assert(normalizedPathKey(truncated) == truncated);
    std::cout << "  (non-ASCII case folding and its boundaries hold)\n";

    // KNOWN GAP, stated rather than hidden: Unicode normalisation.
    // "é" as one code point and "e" + combining acute are the same
    // character and the same filename, and these keys differ. A stick
    // written on macOS carries the decomposed form. Not fixed here
    // because it needs a normalisation table rather than a case rule,
    // and a half-done one would be worse than none.
    const std::string composed = "/Contents/caf\u00e9.mp3";
    const std::string decomposed = "/Contents/cafe\u0301.mp3";
    assert(normalizedPathKey(composed) != normalizedPathKey(decomposed));

    std::cout << "all cases passed\n";
    return 0;
}
