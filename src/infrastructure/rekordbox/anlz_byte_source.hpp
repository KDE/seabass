// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <optional>
#include <string>

namespace seabass::infrastructure::rekordbox
{

// Where a track's analysis-file bytes come from. Analysis files (ANLZ
// .DAT/.EXT under PIONEER/USBANLZ) carry the cues, beatgrid and waveform
// for one track each, and they dominate a library's metadata: measured on
// a real 6,000-track export, 641 MB of ANLZ against 8 MB of databases.
//
// A mounted stick or a folder library reads them straight off the
// filesystem (FilesystemAnlzSource), exactly as this code always did.
// A stick backup browsed in place serves them out of the ZIP instead --
// one seek and one inflate per track, nothing written to disk. That is
// the whole reason this abstraction exists: the databases have to be real
// files because SQLite, SQLCipher and the pdb parser all need a seekable
// path, but they are ~1% of the bytes. ANLZ is the other 99%, it is
// wanted one track at a time, and so it never has to be extracted at all.
//
// Keyed on the path *relative to the PIONEER folder* (e.g.
// "USBANLZ/P05D/000117F3/ANLZ0000.EXT") rather than an absolute path,
// because that is the one form both a filesystem root and an archive
// entry name can be built from. See anlzRelativePath().
class AnlzByteSource
{
public:
    virtual ~AnlzByteSource() = default;

    // The file's whole contents, or nullopt when there is no such
    // analysis file -- a track without one is normal (nothing analysed it
    // yet), and every caller treats it as "no cues"/"no waveform" rather
    // than an error, which is what the old "ifstream did not open" check
    // did too.
    virtual std::optional<std::string> read(const std::string &relativePath) = 0;
};

// Reads from a real PIONEER directory. `pioneerRoot` is the folder itself,
// the same argument every other rekordbox class takes.
class FilesystemAnlzSource : public AnlzByteSource
{
public:
    explicit FilesystemAnlzSource(std::string pioneerRoot);
    std::optional<std::string> read(const std::string &relativePath) override;

private:
    std::string m_pioneerRoot;
};

// The relative form of an `analyze_path` as export.pdb stores it (an
// absolute, stick-rooted path like
// "/PIONEER/USBANLZ/P05D/000117F3/ANLZ0000.DAT"). `wantExt` switches the
// extension to ".EXT", which is where the extended cue sections (PCO2)
// live; the plain ".DAT" holds the waveform preview.
//
// Same string surgery extAnlzPath()/datAnlzPath() do before joining a
// root, factored out so an archive can join an entry prefix instead.
std::string anlzRelativePath(const std::string &analyzePath, bool wantExt);

}  // namespace seabass::infrastructure::rekordbox
