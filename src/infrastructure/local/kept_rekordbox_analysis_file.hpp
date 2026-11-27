// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// rekordbox's own analysis of a track, kept on the stick before Analyse
// again first replaces it, so that Restore original analysis can put it
// back: <stick>/Seabass/rekordbox-analysis/<name>.tsv.gz
// (paths::stickKeptRekordboxAnalysisDir), one file per track. <name> is the
// first 32 hex digits of the SHA-256 of the track's ledger key, as for the
// kept Engine analyses (kept_engine_analysis_file.hpp).
//
// What is kept is the track's analysis files whole, as they were (.DAT,
// .EXT, .2EX, each as its bytes, or "none" where the track had none), and
// the tempo and key of every row of rekordbox's two libraries that Analyse
// again gave its tempo, as the rows stored them. Whole files rather than
// only the sections Analyse again replaces: a restore then needs nothing
// but the kept bytes and the cue lists of the files as they are, and a
// file of a format version nobody has seen yet still goes back as it was.
// The waveforms make up most of it: measured on a rekordbox 7 collection
// (200 tracks), the three files are 320 kB at the median and an entry
// 235 kB once deflated (base64 inside the gzip costs a tenth over
// deflating the bytes alone), up to some 320 kB.
//
// Format version 1: gzip (so `zcat` reads it) around tab-separated text:
//
//   seabass-rekordbox-analysis<TAB>1<TAB>stickPath      (escaped)
//   pathKey<TAB>key                                      (escaped)
//   audioFile<TAB>size<TAB>mtimeUnix
//   originalGrid<TAB>64 hex | -
//   anlz<TAB>datPath<TAB>dat<TAB>ext<TAB>2ex             (one line per file set)
//   row<TAB>library<TAB>id<TAB>tempo<TAB>key<TAB>keyName (one line per row)
//   cue<TAB>library<TAB>kind<TAB>pad<TAB>loop<TAB>oldMs<TAB>newMs<TAB>oldEndMs<TAB>newEndMs
//                                                        (one line per cue moved)
//   #sha256<TAB>hex of every byte above
//
// - stickPath: the audio file from the stick's root, "/Contents/...", as
//   the rows spell it. First, so a listing reads one line of the file.
// - audioFile: the audio file when its analysis was kept, its size in
//   bytes and its modification time in seconds since 1970. The size
//   decides whether a file is the one kept for, as the analysis ledger's
//   classifyAnalysis() decides it; the time is for a person reading it.
// - originalGrid: the fingerprint (rekordbox::pqtzGridFingerprint()) of
//   the grid in the first kept .DAT. The grids Seabass wrote over it are
//   in the entry's record beside it (below), which a later Analyse again
//   rewrites without writing the entry of some 235 kB again.
// - anlz: the .DAT path as the rows name it ("/PIONEER/USBANLZ/.../
//   ANLZ0000.DAT", escaped), then each file as "none" or "b64:" and its
//   bytes in base64.
// - row: "rekordbox" (export.pdb) or "onelibrary" (exportLibrary.db), the
//   row's id, its tempo in hundredths of a BPM and its key's id, each a
//   decimal or "null" where the row held NULL, then the name of the key
//   that id named, "name:" and the name (escaped), or "none" where the id
//   named no key row (NULL, 0, or a dangling id). The id is for a person
//   reading the file; a restore writes the key by its name, since after a
//   new export the same id can name another key.
//
// - cue: a cue point Analyse again moved with the grid, as it wrote it:
//   "rekordbox" (DeviceLibrary's set, which OneLibrary's mirrors, or
//   OneLibrary's alone) or "engine", the kind ("hot" or "memory"), the pad
//   (0 for a memory cue), "1" for a loop, its place before and after the
//   move and a loop's end before and after, in ms (0 for no loop), each
//   the shortest decimal that reads back as the same double. None where
//   Analyse again moved no cue: then Restore moves none. A later Analyse
//   again that moves the cues again carries each line on (its old place
//   stays the one before the first move).
//
// Every line ends with a newline. A reader that meets any other version,
// a line out of order, a bad field or a checksum that does not match
// refuses the file: the entry is Damaged and nothing in it is believed.
//
// The entry's record, <name>.seabass.tsv beside it (the same <name>): what
// Seabass did to the track since it kept the entry, small enough to be
// written again by every save that adds to it. Format version 1, plain
// text:
//
//   seabass-rekordbox-analysis-record<TAB>1
//   pathKey<TAB>key                                      (escaped)
//   origin<TAB>rekordbox | unknown
//   seabassGrids<TAB>64 hex[,64 hex...] | -
//   engineKept<TAB>64 hex | -
//   #sha256<TAB>hex of every byte above
//
// - origin: "rekordbox" where the stick's analysis ledger was whole when
//   the entry was kept, so the grid kept was not Seabass's; "unknown"
//   where the ledger had been moved aside or reset before (it may then be
//   an analysis of Seabass's that the lost ledger knew): the entry is the
//   analysis that was on the stick before, whoever made it.
// - seabassGrids: every grid Seabass wrote into the track's .DAT since the
//   entry was kept, as pqtzGridFingerprint(), each written into the record
//   before the .DAT in the same save. The record alone tells a grid of
//   Seabass's from one changed outside Seabass for its own track, whatever
//   became of the analysis ledger.
// - engineKept: where the Analyse again that kept or added to the entry
//   also kept the player's analysis of the track's Engine row, the
//   fingerprint of that kept analysis (keptEngineOriginalFingerprint());
//   "-" where Analyse again kept none (none was there, or Beat Grids &
//   Waveforms had kept it before: that one is that page's to restore).
//
// An entry with no record, or with one that cannot be read, is one of
// which Seabass cannot tell what is its own: Analyse again leaves the
// track alone and Restore refuses it. The two are written, backed up,
// carried, listed and removed together: the entry first and its record
// after it, removed in that order too.
namespace seabass::infrastructure::local
{

inline constexpr int KeptRekordboxAnalysisFormatVersion = 1;

// One analysis file set of the track, as it was.
struct KeptAnlzFileSet
{
    std::string datPath;
    std::optional<std::string> dat;
    std::optional<std::string> ext;
    std::optional<std::string> twoEx;

    friend bool operator==(const KeptAnlzFileSet &, const KeptAnlzFileSet &) = default;
};

// One row's tempo and key, as stored.
struct KeptRekordboxRow
{
    std::string library;
    std::int64_t rowId = 0;
    std::optional<std::int64_t> tempo;
    std::optional<std::int64_t> keyId;
    std::optional<std::string> keyName;

    friend bool operator==(const KeptRekordboxRow &, const KeptRekordboxRow &) = default;
};

// One cue point Analyse again moved with the grid.
struct KeptCueMove
{
    std::string library;
    bool hot = false;
    int pad = 0;
    bool loop = false;
    double oldMs = 0.0;
    double newMs = 0.0;
    double oldEndMs = 0.0;
    double newEndMs = 0.0;

    friend bool operator==(const KeptCueMove &, const KeptCueMove &) = default;
};

struct KeptRekordboxAnalysis
{
    std::string pathKey;
    std::string stickPath;
    std::uint64_t audioFileSize = 0;
    std::int64_t audioFileMtimeUnix = 0;
    std::string originalGrid;
    std::vector<KeptAnlzFileSet> files;
    std::vector<KeptRekordboxRow> rows;
    std::vector<KeptCueMove> cueMoves;

    friend bool operator==(const KeptRekordboxAnalysis &, const KeptRekordboxAnalysis &) = default;
};

struct KeptRekordboxAnalysisLookup
{
    enum class Status { None, Found, Damaged };
    Status status = Status::None;
    KeptRekordboxAnalysis entry;
    std::string error;
};

// The entry's record (above).
struct KeptRekordboxRecord
{
    std::string pathKey;
    bool originKnown = true;
    std::vector<std::string> seabassGrids;
    std::string engineKept;

    friend bool operator==(const KeptRekordboxRecord &, const KeptRekordboxRecord &) = default;
};

struct KeptRekordboxRecordLookup
{
    enum class Status { None, Found, Damaged };
    Status status = Status::None;
    KeptRekordboxRecord record;
    std::string error;
};

// The file of a track's entry, and of its record.
std::filesystem::path keptRekordboxAnalysisFile(const std::filesystem::path &stickRoot, const std::string &pathKey);
std::filesystem::path keptRekordboxRecordFile(const std::filesystem::path &stickRoot, const std::string &pathKey);
// The record's file beside an entry's file (the same name, the record's
// suffix).
std::filesystem::path keptRekordboxRecordFileBeside(const std::filesystem::path &entryFile);

// The entry's text before compression, and back. parse() gives Damaged
// with `error` for any text that is not this format to the letter.
std::string serializeKeptRekordboxAnalysis(const KeptRekordboxAnalysis &entry);
KeptRekordboxAnalysisLookup parseKeptRekordboxAnalysis(const std::string &text);
std::string serializeKeptRekordboxRecord(const KeptRekordboxRecord &record);
KeptRekordboxRecordLookup parseKeptRekordboxRecord(const std::string &text);

// The store on one stick. Before a file is created, replaced or removed,
// `beforeChange` is called with its path (UTF-8): a save backs it up and
// protects it for the change. If it throws, the file is not touched. A
// kept entry is read back after it is written and compared with what was
// meant; a write that does not read back throws. Every write is durable.
class StickKeptRekordboxAnalysisStore
{
public:
    explicit StickKeptRekordboxAnalysisStore(std::filesystem::path stickRoot) : m_stickRoot(std::move(stickRoot)) {}

    void setBeforeChange(std::function<void(const std::string &file)> hook) { m_beforeChange = std::move(hook); }

    KeptRekordboxAnalysisLookup find(const std::string &pathKey) const;
    KeptRekordboxRecordLookup findRecord(const std::string &pathKey) const;
    // The entry and its record (of the same key), the entry first.
    void keep(const KeptRekordboxAnalysis &entry, const KeptRekordboxRecord &record);
    // The record alone, the entry left as it is.
    void keepRecord(const KeptRekordboxRecord &record);
    // The entry and its record, the entry first. Nothing kept for the key
    // is not an error.
    void drop(const std::string &pathKey);

private:
    std::filesystem::path m_stickRoot;
    std::function<void(const std::string &)> m_beforeChange;
};

// The entry's first four lines, read without the kept bytes after them
// (and so without its checksum, which covers the whole file): what a
// listing needs. nullopt for a file whose head is not this format.
struct KeptRekordboxAnalysisHead
{
    std::string stickPath;
    std::string pathKey;
    std::string originalGrid;
};
std::optional<KeptRekordboxAnalysisHead> readKeptRekordboxAnalysisHead(const std::filesystem::path &file);

// Every grid any record on the stick lists as Seabass's, for a decision
// about a track whose own record and ledger entry say nothing (a track
// moved with neither carried). A record that cannot be read adds none.
// `error` says when the folder cannot be read whole (required).
std::vector<std::string> keptRekordboxSeabassGrids(const std::filesystem::path &stickRoot, std::string *error);

// One kept entry as a listing sees it, without reading the kept bytes:
// the file, its size on the stick, and the track it is of. For the
// rekordbox entries `trackFile` is the stick path (the first line); for
// the Engine ones, the row's Track.path as stored, from the Engine Library
// folder. `readable` is false for a file whose track cannot be read from
// it; such a file is listed all the same, as one whose track is gone.
struct KeptAnalysisFileInfo
{
    std::string file;
    std::uintmax_t bytes = 0;
    std::string trackFile;
    bool readable = false;
    // A rekordbox entry's record, when it is there ("" otherwise): its
    // size is in `bytes`, and it goes with the entry wherever the entry
    // goes. `originKnown` is false where the record says the origin is
    // unknown, or cannot be read.
    std::string record;
    bool originKnown = true;
};

// Every entry in the stick's two kept-analysis folders, a rekordbox entry
// with its record (a record with no entry beside it is not listed: it
// holds no analysis, and the next keep of its track writes it again). A
// folder that is
// not there is an empty list; one that cannot be read gives what was read
// and `error` saying why (required).
std::vector<KeptAnalysisFileInfo> listKeptRekordboxAnalyses(const std::filesystem::path &stickRoot, std::string *error);
std::vector<KeptAnalysisFileInfo> listKeptEngineAnalyses(const std::filesystem::path &stickRoot, std::string *error);

}  // namespace seabass::infrastructure::local
