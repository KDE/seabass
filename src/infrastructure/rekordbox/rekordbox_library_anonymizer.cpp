// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_library_anonymizer.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "infrastructure/anonymization_placeholder.hpp"
#include "infrastructure/anonymization_export_layout.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/onelibrary/onelibrary_anonymizer.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace fs = std::filesystem;
using Pdb = rekordbox_pdb_t;
using Anlz = rekordbox_anlz_t;

namespace
{

// Everything the enumeration pass below needs about one present track
// row -- ids for the writer steps below, analyzePath to locate
// its ANLZ files, and the real filename (read here, before it's
// overwritten) used only as the anonymizationPlaceholder() correlation
// key -- see that header's own comment for why the obfuscated filename
// specifically needs to come out identical to what the Engine-side
// anonymizer independently derives for the same real file.
struct TrackRowInfo
{
    uint32_t id = 0;
    uint32_t artistId = 0;
    std::string analyzePath;  // e.g. "/PIONEER/USBANLZ/P05D/000117F3/ANLZ0000.DAT"
    std::string filename;
};

// One raw, read-only pass over the (already-copied) export.pdb --
// separate from PdbRowWriter, which only supports single-id lookups,
// not enumeration. Mirrors KaitaiRekordboxReader's own table-walking
// pattern, but collects the raw ids/analyzePaths this anonymizer needs
// rather than building domain::Track objects.
struct EnumerationResult
{
    std::vector<TrackRowInfo> tracksInDiskOrder;
    std::vector<uint32_t> artistIds;
    std::vector<uint32_t> playlistIds;  // playlist_tree rows, folders and playlists alike
};

EnumerationResult enumeratePdb(const std::string &pdbPath)
{
    std::ifstream ifs(pdbPath, std::ifstream::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("could not open " + pdbPath);
    }
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);

    EnumerationResult result;
    for (const auto &table : *pdb.tables()) {
        if (table->type() == Pdb::PAGE_TYPE_TRACKS) {
            forEachDataPage(*table, [&](Pdb::page_t *page) {
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (!row->present()) {
                            continue;
                        }
                        auto *t = dynamic_cast<Pdb::track_row_t *>(row->body());
                        if (!t) {
                            continue;
                        }
                        result.tracksInDiskOrder.push_back(
                            TrackRowInfo{t->id(), t->artist_id(), sqlText(t->analyze_path()), sqlText(t->filename())});
                    }
                }
            });
        } else if (table->type() == Pdb::PAGE_TYPE_ARTISTS) {
            forEachDataPage(*table, [&](Pdb::page_t *page) {
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (!row->present()) {
                            continue;
                        }
                        if (auto *a = dynamic_cast<Pdb::artist_row_t *>(row->body())) {
                            result.artistIds.push_back(a->id());
                        }
                    }
                }
            });
        } else if (table->type() == Pdb::PAGE_TYPE_PLAYLIST_TREE) {
            forEachDataPage(*table, [&](Pdb::page_t *page) {
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (!row->present()) {
                            continue;
                        }
                        if (auto *p = dynamic_cast<Pdb::playlist_tree_row_t *>(row->body())) {
                            result.playlistIds.push_back(p->id());
                        }
                    }
                }
            });
        }
    }
    return result;
}

// Deterministic, distinct-per-index placeholder text -- re-running the
// anonymizer against the same source produces the same output, and
// every track gets its own title index, so (title, artist) pairs can
// never collide across two different real tracks even though artist
// names are shared/reused across a real artist's tracks (as they
// should be -- see anonymizeRekordboxLibrary()'s own doc comment).
std::string placeholder(const std::string &kind, size_t index)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03zu", index);
    return kind + " " + buf;
}

// Neither .DAT nor .EXT -- the third, nxs2-only sibling holding 3-band
// color waveform data; there's no existing helper for this one (the
// app doesn't read it), so it's derived the same way datAnlzPath()/
// extAnlzPath() are.
std::string twoExAnlzPath(const std::string &pioneerRoot, const std::string &analyzePath)
{
    std::string rel = analyzePath;
    size_t pos = rel.find("/PIONEER/");
    if (pos != std::string::npos) {
        rel = rel.substr(pos + std::string("/PIONEER/").size());
    }
    size_t dot = rel.rfind(".DAT");
    if (dot != std::string::npos) {
        rel = rel.substr(0, dot) + ".2EX";
    }
    return pioneerRoot + "/" + rel;
}

// Strips every large waveform-detail section this app's own reader
// never touches, from one ANLZ file in place, if it exists and parses
// as a valid ANLZ file -- everything else (cues, beatgrid, path, the
// one waveform tag rekordbox_waveform_reader.cpp actually reads) is
// left untouched. Verified against real captured files (not just the
// spec): WAVE_SCROLL alone ran 55-175KB/track on a real ~1,400-track
// library -- rekordbox_waveform_reader.hpp's own doc comment confirms
// this app only ever reads WAVE_PREVIEW ("the low-resolution monochrome
// waveform preview (the 'PWAV' tag...)"), so WAVE_SCROLL (PWV3, the
// real-time scrolling display rekordbox's own UI uses during playback)
// is exactly as safe to drop as the color/3-band tags, and was the
// single largest remaining contributor to a first version of this
// function that only stripped the color/3-band ones. Silently does
// nothing for a file that doesn't exist (e.g. no .2EX sibling, common
// for tracks never analyzed by nxs2-generation software) or fails to
// parse (defensive; never lets a stripping failure abort the whole
// anonymization run).
bool isLargeUnusedWaveformSection(const AnlzRawSection &s)
{
    return s.fourcc == static_cast<uint32_t>(Anlz::SECTION_TAGS_WAVE_SCROLL) ||
           s.fourcc == static_cast<uint32_t>(Anlz::SECTION_TAGS_WAVE_COLOR_PREVIEW) ||
           s.fourcc == static_cast<uint32_t>(Anlz::SECTION_TAGS_WAVE_COLOR_SCROLL) ||
           s.fourcc == static_cast<uint32_t>(Anlz::SECTION_TAGS_WAVE_3BAND_PREVIEW) ||
           s.fourcc == static_cast<uint32_t>(Anlz::SECTION_TAGS_WAVE_3BAND_SCROLL);
}

// cue_extended_entry's fixed prefix (magic..loop_denominator) per
// specs/rekordbox_anlz.ksy -- len_comment (u4 BE) immediately follows,
// then `len_comment` bytes of UTF-16BE comment text (including a
// trailing NUL terminator) if len_entry > 43.
constexpr size_t CueEntryFixedSize = 40;

// Obfuscates every cue's comment text in place within one CUES_2
// (extended cue list) section's raw bytes -- real free text a DJ typed
// per cue point, which kaitai_rekordbox_reader.cpp's readCues() does
// read from real files even though this project's own cue *writer*
// doesn't write new ones (anlz_cue_codec.hpp's "v1: no comment
// support"), so an untouched CUES_2 section here would leak it.
// Byte-length-preserving, matching PdbRowWriter's own string-overwrite
// discipline: len_comment (and every other field) is never touched,
// only the text itself, still ending in the format's own NUL
// terminator. nextIndex is shared across every track's cues in one
// anonymization run, so placeholders stay distinct.
void obfuscateCueComments(std::string &sectionBytes, size_t &nextIndex)
{
    if (sectionBytes.size() < 20) {
        return;
    }
    uint16_t numCues = readU16BE(sectionBytes, 16);
    size_t offset = 20;
    for (uint16_t i = 0; i < numCues; ++i) {
        if (offset + CueEntryFixedSize + 4 > sectionBytes.size()) {
            return;  // defensive: malformed section, stop rather than read further out of bounds
        }
        uint32_t lenEntry = readU32BE(sectionBytes, offset + 8);
        uint32_t lenComment = (lenEntry > 43) ? readU32BE(sectionBytes, offset + CueEntryFixedSize) : 0;
        size_t commentOffset = offset + CueEntryFixedSize + 4;
        if (lenComment >= 2 && commentOffset + lenComment <= sectionBytes.size()) {
            size_t capacityUnits = lenComment / 2 - 1;  // excludes the trailing NUL terminator
            std::string placeholder = capacityUnits > 0 ? ("Cue " + std::to_string(nextIndex++)) : "";
            std::string fitted = placeholder.substr(0, std::min(placeholder.size(), capacityUnits));
            size_t u = 0;
            for (; u < fitted.size(); ++u) {
                sectionBytes[commentOffset + u * 2] = 0x00;
                sectionBytes[commentOffset + u * 2 + 1] = fitted[u];
            }
            for (; u < capacityUnits; ++u) {  // right-pad with UTF-16BE spaces, same as the ASCII field padding
                sectionBytes[commentOffset + u * 2] = 0x00;
                sectionBytes[commentOffset + u * 2 + 1] = ' ';
            }
            sectionBytes[commentOffset + capacityUnits * 2] = 0x00;  // trailing NUL terminator, preserved
            sectionBytes[commentOffset + capacityUnits * 2 + 1] = 0x00;
        }
        offset += lenEntry;
    }
}

// The PATH section holds the audio file's real path as UTF-16BE text --
// "/Contents/<Artist>/<Album>/NNN_<Artist>-<Title>.mp3" on a typical
// library, so the artist, album and title are all in there even though
// the pdb row's own copies of them were replaced. MANIFEST.txt promises
// contributors that original file paths are removed entirely, and until
// this existed that promise was broken in every single analysis file.
//
// Overwritten in place and byte-length-preserving, exactly like
// obfuscateCueComments() above: len_path is never touched, only the text
// inside it, still ending in the format's own NUL terminator. The
// replacement is derived from the real basename through the same
// anonymizationFilenamePlaceholder() the pdb row and the Engine database
// use, so one real track keeps one placeholder across all three.
void obfuscatePathSection(std::string &sectionBytes)
{
    constexpr size_t LenHeaderOffset = 4;
    constexpr size_t LenPathOffset = 12;
    if (sectionBytes.size() < LenPathOffset + 4) {
        return;
    }
    const uint32_t lenHeader = readU32BE(sectionBytes, LenHeaderOffset);
    const uint32_t lenPath = readU32BE(sectionBytes, LenPathOffset);
    if (lenPath < 4 || lenHeader + lenPath > sectionBytes.size()) {
        return;  // defensive: malformed section, leave it alone
    }
    const size_t capacityUnits = lenPath / 2 - 1;  // excludes the trailing NUL

    std::string realPath;
    for (size_t u = 0; u < capacityUnits; ++u) {
        // The paths this format carries are ASCII in practice; anything
        // wider only affects which basename we hash, never whether the
        // text gets replaced.
        realPath.push_back(sectionBytes[lenHeader + u * 2 + 1]);
    }
    while (!realPath.empty() && (realPath.back() == ' ' || realPath.back() == '\0')) {
        realPath.pop_back();
    }
    const size_t slash = realPath.find_last_of('/');
    const std::string realFilename = slash == std::string::npos ? realPath : realPath.substr(slash + 1);
    const std::string replacement = "/Contents/" + anonymizationFilenamePlaceholder(realFilename);

    const std::string fitted = replacement.substr(0, std::min(replacement.size(), capacityUnits));
    size_t u = 0;
    for (; u < fitted.size(); ++u) {
        sectionBytes[lenHeader + u * 2] = 0x00;
        sectionBytes[lenHeader + u * 2 + 1] = fitted[u];
    }
    for (; u < capacityUnits; ++u) {
        sectionBytes[lenHeader + u * 2] = 0x00;
        sectionBytes[lenHeader + u * 2 + 1] = ' ';
    }
    sectionBytes[lenHeader + capacityUnits * 2] = 0x00;
    sectionBytes[lenHeader + capacityUnits * 2 + 1] = 0x00;
}

// Returns why the file could not be scrubbed, or an empty string when it
// was (or when there is no such file, which is ordinary: a track may
// have a .DAT and no .EXT).
//
// It used to swallow every exception and return. What stays behind then
// is an analysis file in the export with its PATH section intact, and
// that section holds the audio file's real path -- artist, album and
// title, on a typical library -- which MANIFEST.txt promises a
// contributor was removed. The verifier refuses such an export, so
// nothing leaked; but the refusal arrives from the check that does not
// know what went wrong, and says "this export did not verify" instead
// of naming the file it could not scrub.
//
// "Defensive only" was the old comment, which is a statement about what
// cannot happen with nothing enforcing it. A truncated analysis file on
// a real stick is not exotic.
// A path, or a message carrying one, with the export's own root taken
// off the front: what is left is where the file sits INSIDE the export,
// which is what a contributor can act on and all they should be shown.
// The machine's own directory layout is not theirs to paste anywhere.
std::string insideExport(const std::string &text, const std::string &destinationRoot)
{
    std::string out = text;
    for (const std::string &prefix : {destinationRoot + "/", destinationRoot + "\\", destinationRoot}) {
        for (std::size_t at = out.find(prefix); at != std::string::npos; at = out.find(prefix, at)) {
            out.erase(at, prefix.size());
        }
    }
    return out;
}

std::string anonymizeAnlzFile(const std::string &path, size_t &nextCueCommentIndex)
{
    std::error_code ec;
    const bool there = fs::exists(path, ec);
    if (ec) {
        // exists() answers false for both "not there" and "could not
        // look", and only the error code tells them apart. Reported,
        // because a file this could not examine is a file it did not
        // scrub.
        return "could not tell whether it is there: " + ec.message();
    }
    if (!there) {
        return {};
    }
    try {
        AnlzFile file = AnlzFile::readRaw(path);
        bool pathScrubbed = false;
        for (auto &section : file.sections) {
            if (section.fourcc == static_cast<uint32_t>(Anlz::SECTION_TAGS_CUES_2)) {
                obfuscateCueComments(section.rawBytes, nextCueCommentIndex);
            } else if (section.fourcc == static_cast<uint32_t>(Anlz::SECTION_TAGS_PATH)) {
                obfuscatePathSection(section.rawBytes);
                pathScrubbed = true;
            }
        }
        size_t before = file.sections.size();
        file.sections.erase(std::remove_if(file.sections.begin(), file.sections.end(), isLargeUnusedWaveformSection),
                             file.sections.end());
        // Comment obfuscation always mutates in place (same section
        // count either way), so always re-write whenever any CUES_2
        // section exists, not just when stripping actually removed one.
        bool hadCues2 = std::any_of(file.sections.begin(), file.sections.end(), [](const AnlzRawSection &s) {
            return s.fourcc == static_cast<uint32_t>(Anlz::SECTION_TAGS_CUES_2);
        });
        if (file.sections.size() != before || hadCues2 || pathScrubbed) {
            file.writeRaw(path);
        }
    } catch (const std::exception &e) {
        return std::string("could not read or rewrite it: ") + e.what();
    }
    return {};
}

void copyTreeIfPresent(const fs::path &from, const fs::path &to)
{
    std::error_code ec;
    if (!fs::exists(from, ec)) {
        return;
    }
    fs::copy(from, to, fs::copy_options::recursive, ec);
    if (ec) {
        throw std::runtime_error("failed to copy " + from.string() + " to " + to.string() + ": " + ec.message());
    }
}

}  // namespace

RekordboxAnonymizationResult anonymizeRekordboxLibrary(const std::string &sourceRoot, const std::string &destinationRoot,
                                                         bool slimForTesting,
                                                         application::ProgressReporter &reporter)
{
    RekordboxAnonymizationResult result;

    std::error_code ec;
    if (fs::exists(destinationRoot, ec) && !fs::is_empty(destinationRoot, ec)) {
        result.errorMessage = destinationRoot + " already exists and isn't empty -- refusing to write into it";
        return result;
    }
    fs::create_directories(destinationRoot, ec);

    try {
        copyTreeIfPresent(fs::path(sourceRoot) / "rekordbox", fs::path(destinationRoot) / "rekordbox");
        // That copy takes the whole rekordbox/ directory, which on a real
        // stick holds more than export.pdb.
        //
        // Everything in there that is not on the allowlist goes: the
        // .sync playlist state, RBFLTR.DAT and anything a newer
        // rekordbox invents. exportExt.pdb used to be in that list and
        // is not any more -- it is on the allowlist now and scrubbed a
        // few lines below, and removed again if that scrub does not
        // land.
        //
        // A list of files to REMOVE was what this used to be, and a real
        // stick turned up carrying three it had never heard of -- which
        // the verifier then refused, correctly, leaving the user with an
        // export that could not be produced at all. An allowlist cannot
        // fail that way: an unknown file is dropped, not shipped.
        {
            std::error_code listEc;
            const fs::path catalogDir = fs::path(destinationRoot) / "rekordbox";
            std::vector<fs::path> unknown;
            for (const auto &entry : fs::directory_iterator(catalogDir, listEc)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                const std::string name = entry.path().filename().string();
                if (!isKeptRekordboxCatalogFile(name)) {
                    unknown.push_back(entry.path());
                }
            }
            for (const auto &path : unknown) {
                // Counted as dropped only once it is actually gone. This
                // list is what the manifest tells whoever receives the
                // export; a file still present that the manifest calls
                // removed is the one outcome this whole class exists to
                // prevent.
                std::string failure;
                if (infrastructure::removeEntry(path, failure)) {
                    result.removedUnanonymizableFiles.push_back(path.filename().string());
                } else {
                    result.unremovedUnanonymizableFiles.push_back(path.filename().string() + ": " + failure);
                }
            }
        }
        copyTreeIfPresent(fs::path(sourceRoot) / "USBANLZ", fs::path(destinationRoot) / "USBANLZ");
        // Device Profile reads these and nothing else does. They hold
        // player preferences (LCD brightness, quantize, jog feel), not
        // anything about the person or their music, so they go in as-is --
        // and without them a donated set cannot exercise that feature at
        // all. djprofile.nxs is deliberately not among them: it is a
        // device profile blob this app never reads and has not been
        // audited for identifying content.
        for (const char *settingsFile : {"MYSETTING.DAT", "MYSETTING2.DAT", "DEVSETTING.DAT", "DJMMYSETTING.DAT"}) {
            std::error_code settingsEc;
            const fs::path from = fs::path(sourceRoot) / settingsFile;
            if (fs::exists(from, settingsEc)) {
                fs::copy_file(from, fs::path(destinationRoot) / settingsFile,
                              fs::copy_options::overwrite_existing, settingsEc);
                if (!settingsEc) {
                    ++result.deviceSettingsFilesCopied;
                }
            }
        }

        std::string pdbPath = (fs::path(destinationRoot) / "rekordbox" / "export.pdb").string();
        EnumerationResult enumerated = enumeratePdb(pdbPath);

        // Every analysis file this run has already been through, so the sweep

        // below does not process one twice (which would re-hash an

        // already-anonymized name into a different one).

        std::set<std::string> visitedAnlz;


        reporter.start("Anonymizing rekordbox library", enumerated.tracksInDiskOrder.size());

        const std::vector<TrackRowInfo> &tracks = enumerated.tracksInDiskOrder;

        // exportLibrary.db is the Device Library Plus mirror: the complete
        // real library, encrypted with a key this project's own source
        // derives, so anyone with the app can read it straight out. It
        // used to ship verbatim, then it was dropped outright, which was
        // safe but left the OneLibrary write path with no real-data
        // coverage at all. Now it is scrubbed and kept. A failure removes
        // it rather than shipping it: this file is the worst one to get
        // wrong.
        {
            const fs::path oneLibrary = fs::path(destinationRoot) / "rekordbox" / "exportLibrary.db";
            std::error_code existsEc;
            if (fs::is_regular_file(oneLibrary, existsEc)) {
                auto oneLibraryResult = onelibrary::anonymizeOneLibraryDatabase(oneLibrary.string());
                result.oneLibraryTracksScrubbed = oneLibraryResult.tracksScrubbed;
                result.oneLibraryError = oneLibraryResult.errorMessage;
                if (!oneLibraryResult.errorMessage.empty()) {
                    // The scrub failed, so this database still holds real
                    // titles, artists and paths; dropping it is what keeps
                    // the export shareable. Not a sidecar and not
                    // cleanup: if it cannot be dropped, the export is
                    // unsafe and has to say so.
                    std::string failure;
                    if (!infrastructure::removeEntry(oneLibrary, failure)) {
                        result.unremovedUnanonymizableFiles.push_back(
                            oneLibrary.filename().string() + " (unscrubbed Device Library Plus mirror): " + failure);
                    }
                }
            }
            // Only now: SQLite recreates its -shm and -wal side files the
            // moment the database is opened, so removing them before the
            // scrub above just means they come back holding whatever the
            // scrub itself wrote. They are removed last, and the VACUUM
            // the anonymizer ends with has already folded everything into
            // the database file proper.
            //
            // Left on the raw call deliberately, unlike the three sites
            // above: these two names are fixed ASCII that this code
            // writes itself, so no decomposition to trip over, and their
            // contents post-VACUUM are the scrubbed bytes rather than the
            // originals. One left behind is untidy, not a leak, and the
            // export verifier's byte sweep would find it anyway.
            for (const char *sideFile : {"exportLibrary.db-shm", "exportLibrary.db-wal"}) {
                std::error_code removeEc;
                fs::remove(fs::path(destinationRoot) / "rekordbox" / sideFile, removeEc);
            }
        }
        PdbRowWriter writer(pdbPath);

        std::unordered_set<uint32_t> artistIds;
        size_t trackIndex = 0;
        for (const auto &t : tracks) {
            artistIds.insert(t.artistId);
            // Filename keyed off the real filename via
            // anonymizationFilenamePlaceholder() (not a per-run
            // sequential index) so the same real track gets the same
            // obfuscated filename here and in the independently-run
            // Engine anonymizer -- see that function's own comment for
            // why that's what domain::TrackMatcher's cross-catalog sync
            // matching actually needs to keep working against anonymized
            // data.
            std::string obfuscatedFilename = anonymizationFilenamePlaceholder(t.filename);
            PdbRowWriter::TrackTextOverride text;
            text.title = anonymizationPlaceholder("Track", t.filename);
            text.comment = anonymizationPlaceholder("Comment", t.filename);
            text.filename = obfuscatedFilename;
            text.filePath = "/Contents/" + obfuscatedFilename;
            // Checked, because a refusal here leaves the row's REAL
            // title and comment in the export. Before the writer had a
            // guard this could only fail by not finding the row; now it
            // can also refuse text it cannot represent, and the caller
            // that ignores the answer is the one that ships the leak.
            if (!writer.overwriteTrackText(t.id, text)) {
                // Which of the two it was. Both leave the row
                // unscrubbed, so both belong here, but they are
                // different faults: a row that is not there means the
                // file changed under the scan that listed it, and a
                // refusal means a placeholder this writer cannot
                // represent.
                result.rowsNotAnonymized.push_back(
                    "track row " + std::to_string(t.id)
                    + (writer.trackExists(t.id) ? " (replacement text refused)" : " (row no longer in the file)"));
            }

            // The row's other free-text slots. Emptied rather than given
            // placeholders: nothing in this project reads them, and a
            // mix name or an ISRC says what the real recording was.
            PdbRowWriter::TrackExtraTextOverride extra;
            if (!writer.overwriteTrackExtraText(t.id, extra)) {
                result.rowsNotAnonymized.push_back("track row " + std::to_string(t.id) + " (ISRC, texter, message, mix name)");
            }
            ++trackIndex;
            reporter.tick(trackIndex);
        }

        // Rename every distinct artist referenced by a track --
        // once per artist, not once per track, since real libraries
        // routinely have many tracks sharing one artist.
        std::vector<uint32_t> sortedArtistIds(artistIds.begin(), artistIds.end());
        std::sort(sortedArtistIds.begin(), sortedArtistIds.end());
        for (size_t i = 0; i < sortedArtistIds.size(); ++i) {
            if (writer.overwriteArtistName(sortedArtistIds[i], placeholder("Artist", i))) {
                ++result.artistsRenamed;
            }
        }

        // Every playlist/folder row gets renamed, whatever it holds.
        for (size_t i = 0; i < enumerated.playlistIds.size(); ++i) {
            if (writer.overwritePlaylistName(enumerated.playlistIds[i], placeholder("Playlist", i))) {
                ++result.playlistsRenamed;
            }
        }

        // The three name tables nothing above reaches. Every row in each
        // is a name and all of them are real, so they go wholesale rather
        // than per-id like artists: the export shipped every album title,
        // genre and record label until the byte sweep found them.
        //
        // Unlike artists these are not limited to what tracks reference:
        // rows nothing refers to any more stay in the file, so scrubbing
        // only the referenced ones would leave the rest readable.
        result.albumsRenamed = writer.overwriteAllNames(
            PdbRowWriter::NameTable::Albums, [](size_t i) { return placeholder("Album", i); });
        result.genresRenamed = writer.overwriteAllNames(
            PdbRowWriter::NameTable::Genres, [](size_t i) { return placeholder("Genre", i); });
        result.labelsRenamed = writer.overwriteAllNames(
            PdbRowWriter::NameTable::Labels, [](size_t i) { return placeholder("Label", i); });

        // And then artists and playlists wholesale, over the top of the
        // per-id passes above. Those rename what a track refers to;
        // an artist row nothing refers to any more, or a playlist row
        // outside the enumerated tree, is never reached by them and kept
        // its real name -- 15 artists and 2 playlists of a real library
        // survived every other scrub in this file.
        result.artistsRenamed = std::max(
            result.artistsRenamed,
            writer.overwriteAllNames(PdbRowWriter::NameTable::Artists,
                                     [](size_t i) { return placeholder("Artist", i); }));
        result.playlistsRenamed = std::max(
            result.playlistsRenamed,
            writer.overwriteAllNames(PdbRowWriter::NameTable::Playlists,
                                     [](size_t i) { return placeholder("Playlist", i); }));

        // Last edit of all, after every overwrite above: clear what the
        // format itself considers free. Everything scrubbed so far was
        // scrubbed in the live rows; this is where the copies rekordbox
        // left behind when it edited those rows still sit.
        result.freeBytesZeroed = writer.zeroUnusedSpace();

        // Every pass above that can change a byte, not just some of them.
        // The wholesale artist and playlist renames and the free-space
        // clearing all edit the buffer; leaving them out meant a pdb with no
        // track rows but real playlist names, or freed text, was never
        // committed, and the copy went out exactly as rekordbox left it --
        // which the byte sweep then refused with nothing to say why.
        bool anyEditAttempted = !tracks.empty() || !sortedArtistIds.empty() ||
                                 !enumerated.playlistIds.empty() || result.albumsRenamed > 0 ||
                                 result.genresRenamed > 0 || result.labelsRenamed > 0 ||
                                 result.artistsRenamed > 0 || result.playlistsRenamed > 0 ||
                                 result.freeBytesZeroed > 0;
        // Read before commit(), because commit() is the end of this
        // writer's life and the count is about what it wrote. Added to,
        // never assigned, for the same reason freeBytesZeroed is: the
        // exportExt.pdb writer below has its own, and assigning here
        // would drop it and under-state what was cut.
        result.placeholdersTruncated += static_cast<int>(writer.truncatedTextFields());
        if (anyEditAttempted && !writer.commit()) {
            result.errorMessage = "failed to commit anonymized export.pdb (see PdbRowWriter::commit())";
            return result;
        }

        // exportExt.pdb: the My Tag vocabulary (issue #1). Kept now that
        // its names can be overwritten, so a donated set can exercise My
        // Tags at all -- but kept ONLY if the overwrite actually lands.
        //
        // The failure path is the whole point. Every other file here is
        // either scrubbed or dropped by the allowlist above; this one is
        // on the allowlist and scrubbed afterwards, so a scrub that threw
        // or would not commit would leave a file sitting in the export
        // with every real tag name in it, past a verifier that had been
        // told the name was fine. So anything short of a committed
        // rewrite deletes the file and reports it as dropped, which is
        // exactly the behaviour it had before it had an anonymizer.
        //
        // An absent file is not a failure: rekordbox only writes one when
        // the library has My Tags.
        {
            const fs::path extPdb = fs::path(destinationRoot) / "rekordbox" / "exportExt.pdb";
            std::error_code extEc;
            if (fs::exists(extPdb, extEc)) {
                bool scrubbed = false;
                try {
                    PdbRowWriter extWriter(extPdb.string(), PdbRowWriter::Format::ExportExt);
                    int leftAlone = 0;
                    const int renamed =
                        extWriter.overwriteAllTagNames([](size_t i) { return placeholder("Tag", i); }, &leftAlone);
                    // The same pass export.pdb gets, and for the same
                    // reason: overwriteAllTagNames() rewrites the LIVE
                    // rows, and rekordbox leaves the old bytes behind in
                    // page slack when a tag is renamed or deleted. Without
                    // this, a DJ who renamed a My Tag ships the old name.
                    // The committed fixture has no dead tag rows, which is
                    // why its absence went unnoticed.
                    // Added to the same total export.pdb's sweep reports.
                    // Discarding it meant the manifest under-stated what
                    // had been swept, on the one file whose slack this
                    // series added the sweep for.
                    result.freeBytesZeroed += extWriter.zeroUnusedSpace();
                    // And the tag names this writer had to cut, on the
                    // same total, for the same reason.
                    result.placeholdersTruncated += static_cast<int>(extWriter.truncatedTextFields());
                    // No rows means nothing was rewritten, and an empty
                    // vocabulary and a file this code could not read look
                    // identical from here. Treated as a failure, because
                    // guessing "it was empty" is how a real one ships.
                    // EVERY row, not merely some. A row the rewrite
                    // could not touch keeps the My Tag name a DJ typed,
                    // and `renamed > 0` is true of 27 rewritten out of
                    // 28 -- so the one real name would ship inside a
                    // file this code had just declared scrubbed. Same
                    // shape as a refused track row leaving its real
                    // title: a guard that only asks whether SOMETHING
                    // was written cannot see what was left behind.
                    //
                    // Not left to the verifier's byte sweep, which would
                    // catch the name downstream. Relying on the backstop
                    // is how the guard comes to be written this way in
                    // the first place, and the export would fail with a
                    // leak report rather than this saying which rows it
                    // could not do.
                    scrubbed = renamed > 0 && leftAlone == 0 && extWriter.commit();
                    if (renamed > 0 && leftAlone > 0) {
                        result.rowsNotAnonymized.push_back(std::to_string(leftAlone)
                                                           + " My Tag row(s) kept their real names");
                    }
                    if (scrubbed) {
                        result.tagsRenamed = renamed;
                    }
                } catch (const std::exception &) {
                    scrubbed = false;
                }
                if (!scrubbed) {
                    std::string failure;
                    if (infrastructure::removeEntry(extPdb, failure)) {
                        result.removedUnanonymizableFiles.push_back("exportExt.pdb");
                    } else {
                        result.unremovedUnanonymizableFiles.push_back("exportExt.pdb: " + failure);
                    }
                }
            }
        }

        size_t nextCueCommentIndex = 0;
        // Scrub it, and if that cannot be done, drop it -- the same
        // shape as every other unanonymizable file here (export.pdb,
        // exportExt.pdb, the Database2 strays): removed and reported as
        // removed, and only on the "still in this export" list when the
        // removal ALSO failed. Listing it without removing it would have
        // made one truncated analysis file produce no export at all,
        // since AnonymizeLibrary deletes the staging tree whenever that
        // list is not empty. An analysis file is derived data; the
        // export is worth more than the waveform.
        //
        // Named by its path inside the export, never by e.what() alone:
        // rekordbox calls every one of them ANLZ0000.DAT, so the
        // directory is the only identifying part, and AnlzFile's
        // messages carry the absolute destination path, which ends up in
        // MANIFEST.txt and in whatever a contributor pastes into a bug
        // thread.
        auto scrubOrDrop = [&](const std::string &anlz, const char *what) {
            const std::string failure = anonymizeAnlzFile(anlz, nextCueCommentIndex);
            if (failure.empty()) {
                return true;
            }
            const std::string shown = insideExport(anlz, destinationRoot);
            const std::string why = insideExport(failure, destinationRoot);
            std::string removalFailure;
            if (infrastructure::removeEntry(anlz, removalFailure)) {
                result.removedUnanonymizableFiles.push_back(shown + " (" + what + ", could not be scrubbed: " + why
                                                            + ")");
            } else {
                result.unremovedUnanonymizableFiles.push_back(shown + " (" + what
                                                              + ", still holds the real path: " + why
                                                              + ") and could not be removed: " + removalFailure);
            }
            return false;
        };

        for (const auto &t : tracks) {
            if (t.analyzePath.empty()) {
                continue;
            }
            for (const std::string &anlz : {datAnlzPath(destinationRoot, t.analyzePath),
                                            extAnlzPath(destinationRoot, t.analyzePath),
                                            twoExAnlzPath(destinationRoot, t.analyzePath)}) {
                // Two rows can name one analysis file, and scrubbing it
                // twice would report one failure twice and inflate a
                // count a reader trusts.
                if (visitedAnlz.count(anlz) > 0) {
                    continue;
                }
                scrubOrDrop(anlz, "analysis file");
                visitedAnlz.insert(anlz);
            }
        }

        reporter.finish();
        result.tracksAnonymized = static_cast<int>(tracks.size());
        // A stick accumulates analysis files for tracks that were later
        // deleted from the library: rekordbox leaves them behind, and the
        // copy above brings them along. They are not reachable from any
        // present track row, so the loop never saw them -- and every one
        // still held its real path. Verified on a real stick: 827 of 1983
        // triples were orphans of exactly this kind.
        std::error_code sweepEc;
        const fs::path anlzRoot = fs::path(destinationRoot) / "USBANLZ";
        if (fs::is_directory(anlzRoot, sweepEc)) {
            for (const auto &entry : fs::recursive_directory_iterator(anlzRoot, sweepEc)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                const std::string ext = entry.path().extension().string();
                if (ext != ".DAT" && ext != ".EXT" && ext != ".2EX") {
                    continue;
                }
                if (visitedAnlz.count(entry.path().string()) > 0) {
                    continue;
                }
                if (slimForTesting) {
                    // Removed rather than scrubbed. Nothing in the export
                    // points at it, and for a small fixture these are the
                    // bulk of the bytes.
                    // Counted only when gone. These still carry the real
                    // path of a track deleted from the library, so one
                    // left behind is a leak, not a tidiness problem.
                    std::string failure;
                    if (infrastructure::removeEntry(entry.path(), failure)) {
                        ++result.orphanedAnalysisFilesRemoved;
                    } else {
                        result.unremovedUnanonymizableFiles.push_back(
                            insideExport(entry.path().string(), destinationRoot)
                            + " (orphaned analysis file): " + failure);
                    }
                    continue;
                }
                if (!scrubOrDrop(entry.path().string(), "orphaned analysis file")) {
                    continue;
                }
                ++result.orphanedAnalysisFilesScrubbed;
            }
        }
    } catch (const std::exception &e) {
        result.errorMessage = e.what();
    }
    // Said in errorMessage as well as in the list, because every caller
    // checks errorMessage and an export holding a file nothing can scrub
    // is a failed anonymization however much of the rest worked.
    if (!result.unremovedUnanonymizableFiles.empty() && result.errorMessage.empty()) {
        result.errorMessage = std::to_string(result.unremovedUnanonymizableFiles.size())
            + " file(s) that cannot be anonymized are still in the export and could not be removed: "
            + result.unremovedUnanonymizableFiles.front()
            + (result.unremovedUnanonymizableFiles.size() > 1 ? ", ..." : "")
            + " -- this export must not be shared.";
    }
    return result;
}

}  // namespace seabass::infrastructure::rekordbox
