// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "application/ports/library_reader.hpp"
#include "domain/rekordbox_baseline.hpp"
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

// One track's cues from its analysis files, keyed by the path a catalog
// names for them rather than by a catalog's own row id: export.pdb's
// analyze_path and exportLibrary.db's content.analysisDataFilePath are the
// same kind of string ("/PIONEER/USBANLZ/P06D/0001F5E1/ANLZ0000.DAT") and
// often name the same file. Reads the .EXT's lists and the .DAT's legacy
// ones, exactly as KaitaiRekordboxReader::fillCues() does for its own rows.
//
// nullopt when there is no .EXT (a track nothing analysed). Throws
// AnalysisFileUnreadable when the .EXT does not parse, which is that one
// track's problem; a damaged legacy list is warned about and skipped, as
// in the reader. Anything the byte source itself throws (a browsed
// backup's archive that cannot be read) passes through as it is: that is
// the whole read's problem, not one track's.
class AnalysisFileUnreadable : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};
//
// With `cueLists`, also compares the two generations of cue list in the
// bytes just read, the way the cue list audit does
// (compareCueSections), and says what it found there: no file read twice.
std::optional<std::vector<domain::CuePoint>> readAnalysisFileCues(AnlzByteSource &source,
                                                                 const std::string &analyzePath,
                                                                 domain::Track::CueListsCheck *cueLists = nullptr);

// The .EXT's mtime in Unix seconds: the one file that moves when this
// track's cues change and for no other track. nullopt when it cannot be
// stat'd (no such file, or bytes served from an archive).
std::optional<std::int64_t> analysisFileModifiedAt(const std::string &pioneerRoot, const std::string &analyzePath);

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

    // readTracks() + fillCues(), with one progress pass over the tracks
    // under the label this reader always used.
    std::vector<domain::Track> readAll() override;
    // The rows of export.pdb's tracks table, summed from its page
    // headers: the same count readTracks() announces.
    std::optional<size_t> countTracks() override;

    // export.pdb alone: every field but the cues and metadataModifiedAt,
    // which live in the per-track ANLZ files. No audio file is stat'd
    // (see application::fillFileSizes) and no ANLZ file is opened.
    std::vector<domain::Track> readTracks() override;

    // The ANLZ pass over `tracks`: sets each rekordbox track's cues and its
    // metadataModifiedAt (the .EXT's mtime), matched to its catalog row by
    // sourceId. Works on any reader for the same root: one that did not
    // run readTracks() reads the analysis paths from export.pdb first.
    // Tracks of another format, or with no analysis file, are left alone.
    void fillCues(std::vector<domain::Track> &tracks) override;

private:
    std::vector<domain::Track> readCatalog(application::ProgressReporter &progress);
    void readAnalysis(std::vector<domain::Track> &tracks, application::ProgressReporter &progress,
                      const std::string &label);
    // Track id (as sourceId) -> export.pdb's analyze_path, for readAnalysis.
    std::unordered_map<std::string, std::string> analyzePathsFromPdb() const;

    std::string m_pioneerRoot;
    std::shared_ptr<AnlzByteSource> m_anlzSource;
    // Filled by readCatalog(), so a fillCues() after readTracks() on the
    // same reader does not parse export.pdb a second time.
    std::unordered_map<std::string, std::string> m_analyzePathBySourceId;
};

// export.pdb's whole playlist tree, folders included, as baselineFrom and
// the Sync after Rekordbox Export planner take it: each playlist's and
// folder's id and its full path ("Folder/List"), spelled as the reader
// spells PlaylistMembership::name, sorted by id. Throws when export.pdb
// cannot be opened.
std::vector<domain::PlaylistInfo> rekordboxPlaylistTree(const std::string &pioneerRoot);

// Every playlist's id in export.pdb by its full path ("Folder/List"),
// spelled as the reader spells PlaylistMembership::name. Folders are
// left out. Throws when export.pdb cannot be opened.
std::map<std::string, uint32_t> rekordboxPlaylistIdsByPath(const std::string &pioneerRoot);

}  // namespace seabass::infrastructure::rekordbox
