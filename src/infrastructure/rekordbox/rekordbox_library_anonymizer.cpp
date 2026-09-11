// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_library_anonymizer.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "infrastructure/anonymization_placeholder.hpp"
#include "infrastructure/anonymization_export_layout.hpp"
#include "infrastructure/onelibrary/onelibrary_anonymizer.hpp"
#include "infrastructure/long_paths.hpp"
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
// row -- ids for the writer/pruning steps above, analyzePath to locate
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

struct PlaylistEntryInfo
{
    uint32_t playlistId = 0;
    uint32_t trackId = 0;
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
    std::vector<PlaylistEntryInfo> playlistEntries;
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
        } else if (table->type() == Pdb::PAGE_TYPE_PLAYLIST_ENTRIES) {
            forEachDataPage(*table, [&](Pdb::page_t *page) {
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (!row->present()) {
                            continue;
                        }
                        if (auto *e = dynamic_cast<Pdb::playlist_entry_row_t *>(row->body())) {
                            result.playlistEntries.push_back(PlaylistEntryInfo{e->playlist_id(), e->track_id()});
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

void anonymizeAnlzFile(const std::string &path, size_t &nextCueCommentIndex)
{
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return;
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
    } catch (const std::exception &) {
        // Defensive only -- see comment above.
    }
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
                                                         std::optional<size_t> maxTracks,
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
        // Everything in there that is not on the allowlist goes,
        // including exportExt.pdb (the My Tag vocabulary: free text a DJ
        // typed, no anonymizer), the .sync playlist state and RBFLTR.DAT.
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
                std::error_code removeEc;
                fs::remove(path, removeEc);
                result.removedUnanonymizableFiles.push_back(path.filename().string());
            }
        }
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
                    std::error_code removeEc;
                    fs::remove(oneLibrary, removeEc);
                }
            }
            // Only now: SQLite recreates its -shm and -wal side files the
            // moment the database is opened, so removing them before the
            // scrub above just means they come back holding whatever the
            // scrub itself wrote. They are removed last, and the VACUUM
            // the anonymizer ends with has already folded everything into
            // the database file proper.
            for (const char *sideFile : {"exportLibrary.db-shm", "exportLibrary.db-wal"}) {
                std::error_code removeEc;
                fs::remove(fs::path(destinationRoot) / "rekordbox" / sideFile, removeEc);
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

        std::vector<TrackRowInfo> kept = enumerated.tracksInDiskOrder;
        std::vector<TrackRowInfo> dropped;
        if (maxTracks && kept.size() > *maxTracks) {
            auto splitPoint = kept.begin() + static_cast<std::vector<TrackRowInfo>::difference_type>(*maxTracks);
            dropped.assign(splitPoint, kept.end());
            kept.resize(*maxTracks);
        }

        PdbRowWriter writer(pdbPath);

        std::set<uint32_t> droppedIds;
        for (const auto &t : dropped) {
            droppedIds.insert(t.id);
        }
        for (const auto &t : dropped) {
            writer.removeTrack(t.id);
        }
        for (const auto &e : enumerated.playlistEntries) {
            if (droppedIds.count(e.trackId)) {
                writer.removePlaylistEntry(e.playlistId, e.trackId);
            }
        }

        std::unordered_set<uint32_t> keptArtistIds;
        size_t trackIndex = 0;
        for (const auto &t : kept) {
            keptArtistIds.insert(t.artistId);
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
            writer.overwriteTrackText(t.id, text);
            ++trackIndex;
            reporter.tick(trackIndex);
        }

        // Rename every distinct artist referenced by a kept track --
        // once per artist, not once per track, since real libraries
        // routinely have many tracks sharing one artist.
        std::vector<uint32_t> sortedArtistIds(keptArtistIds.begin(), keptArtistIds.end());
        std::sort(sortedArtistIds.begin(), sortedArtistIds.end());
        for (size_t i = 0; i < sortedArtistIds.size(); ++i) {
            if (writer.overwriteArtistName(sortedArtistIds[i], placeholder("Artist", i))) {
                ++result.artistsRenamed;
            }
        }

        // Playlist/folder *structure* is never pruned (see this file's
        // header comment) -- every playlist_tree row gets renamed
        // regardless of whether any of its member tracks survived.
        for (size_t i = 0; i < enumerated.playlistIds.size(); ++i) {
            if (writer.overwritePlaylistName(enumerated.playlistIds[i], placeholder("Playlist", i))) {
                ++result.playlistsRenamed;
            }
        }

        bool anyEditAttempted = !kept.empty() || !dropped.empty() || !sortedArtistIds.empty() ||
                                 !enumerated.playlistIds.empty();
        if (anyEditAttempted && !writer.commit()) {
            result.errorMessage = "failed to commit anonymized export.pdb (see PdbRowWriter::commit())";
            return result;
        }

        size_t nextCueCommentIndex = 0;
        for (const auto &t : kept) {
            if (t.analyzePath.empty()) {
                continue;
            }
            anonymizeAnlzFile(datAnlzPath(destinationRoot, t.analyzePath), nextCueCommentIndex);
            anonymizeAnlzFile(extAnlzPath(destinationRoot, t.analyzePath), nextCueCommentIndex);
            anonymizeAnlzFile(twoExAnlzPath(destinationRoot, t.analyzePath), nextCueCommentIndex);
            visitedAnlz.insert(datAnlzPath(destinationRoot, t.analyzePath));
            visitedAnlz.insert(extAnlzPath(destinationRoot, t.analyzePath));
            visitedAnlz.insert(twoExAnlzPath(destinationRoot, t.analyzePath));
        }

        for (const auto &t : dropped) {
            if (t.analyzePath.empty()) {
                continue;
            }
            fs::path anlzDir = fs::path(datAnlzPath(destinationRoot, t.analyzePath)).parent_path();
            // Not fs::remove_all: this path comes from the library's own
            // analysis paths under a caller-supplied root, so it can be
            // past MAX_PATH, where remove_all spins forever instead of
            // failing. See infrastructure/long_paths.hpp.
            removeTreeDeepestFirst(anlzDir);
        }

        reporter.finish();
        result.tracksKept = static_cast<int>(kept.size());
        result.tracksDropped = static_cast<int>(dropped.size());
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
                    std::error_code removeEc;
                    fs::remove(entry.path(), removeEc);
                    ++result.orphanedAnalysisFilesRemoved;
                    continue;
                }
                anonymizeAnlzFile(entry.path().string(), nextCueCommentIndex);
                ++result.orphanedAnalysisFilesScrubbed;
            }
        }
    } catch (const std::exception &e) {
        result.errorMessage = e.what();
    }
    return result;
}

}  // namespace seabass::infrastructure::rekordbox
