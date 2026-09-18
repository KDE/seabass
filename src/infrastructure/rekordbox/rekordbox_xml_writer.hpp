// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include "domain/rekordbox_xml.hpp"
#include "domain/track.hpp"

namespace seabass::infrastructure::rekordbox
{

// Writes a rekordbox "collection XML" (the DJ_PLAYLISTS document rekordbox
// imports under Preferences -> View -> Layout -> rekordbox xml).
//
// This is the only way into rekordbox on a computer. Every other format
// Seabass speaks is a USB export: export.pdb, exportLibrary.db and Engine's
// m.db all describe a stick, and rekordbox will not import any of them into
// its own library. XML is the documented door, so a library assembled from
// sticks -- with the cues off the Denon side merged in, which is the whole
// point -- reaches the desktop through here or not at all.
//
// Read-only with respect to everything else: it takes tracks and returns a
// string. Nothing here opens a stick, and nothing here writes a file.
struct RekordboxXmlOptions
{
    // Goes into <PRODUCT>. rekordbox shows it in the import pane.
    std::string productName = "Seabass";
    std::string productVersion = "0.1";
    std::string productCompany = "Seabass";
};

// The document as a string, ready to write out. UTF-8, no BOM -- rekordbox
// rejects a BOM here.
std::string writeRekordboxXml(const std::vector<domain::Track> &tracks,
                                const domain::XmlPlaylistNode &playlists,
                                const RekordboxXmlOptions &options = {});

// Exposed for direct unit-test coverage, because both have exactly one
// right answer and getting either subtly wrong produces a file rekordbox
// imports with no complaint and silently mangles.

// XML text escaping: &<>"' -- and nothing else. In particular UTF-8 passes
// through untouched, including the emoji and accented characters that real
// playlist names carry ("Einaudi 🌻", "Amelie Lens' track IDs").
std::string escapeXmlText(const std::string &text);

// A native absolute path as rekordbox's Location attribute:
// "file://localhost" + the path with every byte outside the unreserved set
// percent-encoded, '/' kept as the separator. Windows paths come in with
// backslashes and a drive letter and go out as "file://localhost/C:/...".
std::string toRekordboxLocation(const std::string &absolutePath);

}  // namespace seabass::infrastructure::rekordbox
