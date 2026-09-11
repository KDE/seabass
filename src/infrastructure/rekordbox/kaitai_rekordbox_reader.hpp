// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "application/ports/library_reader.hpp"
#include "domain/track.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"

namespace seabass::infrastructure::rekordbox
{

// The same "no RGB -> fall back to legacy color_id, but color_id == 0
// means no color at all" logic KaitaiRekordboxReader uses internally,
// pulled out as a free function of plain values (rather than the
// Kaitai-generated cue_extended_entry_t, which isn't practical to
// construct outside a real parsed file) so it has direct unit test
// coverage. See kaitai_rekordbox_reader.cpp's own comment on why
// color_id == 0 -> "" specifically matters: it's what makes an
// uncolored rekordbox cue compare equal to an uncolored Engine cue in
// domain::cueSetsEqual().
std::string rekordboxCueColor(bool hasRgb, unsigned char r, unsigned char g, unsigned char b, int colorId);

// Reads a rekordbox USB export (PIONEER/rekordbox/export.pdb +
// PIONEER/USBANLZ/**/ANLZ*.{DAT,EXT}) using the Kaitai-generated parser in
// infrastructure/rekordbox/generated/, built from crate-digger's specs
// (see specs/README.md). This is the only place that knows about the Kaitai
// runtime or the on-disk rekordbox format.
class KaitaiRekordboxReader : public application::LibraryReader
{
public:
    // pioneerRoot is the directory containing "rekordbox/" and "USBANLZ/"
    // (i.e. the "PIONEER" folder itself).
    explicit KaitaiRekordboxReader(std::string pioneerRoot);

    // Same, but with the per-track analysis files coming from somewhere
    // other than that directory -- a stick backup read in place serves
    // them out of the archive (see AnlzByteSource). export.pdb itself
    // still has to be a real file at pioneerRoot, because the Kaitai
    // parser seeks all over it.
    KaitaiRekordboxReader(std::string pioneerRoot, std::shared_ptr<AnlzByteSource> anlzSource);

    std::vector<domain::Track> readAll() override;

private:
    std::string m_pioneerRoot;
    std::shared_ptr<AnlzByteSource> m_anlzSource;
};

}  // namespace seabass::infrastructure::rekordbox
