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
//   seabassGrids<TAB>64 hex[,64 hex...] | -
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
//   the grid in the first kept .DAT; seabassGrids: the grid the Analyse
//   again that kept the entry wrote into the track's .DAT, in the same
//   form. Written with the entry and never added to afterwards: the grids
//   of later Analyse agains are in the analysis ledger, and an entry of
//   some 235 kB is not written again (and backed up again) to note one.
//   A grid that is one of these, in any entry on the stick, is Seabass's
//   (keptRekordboxSeabassGrids()).
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
    std::vector<std::string> seabassGrids;
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

// The file of a track's entry.
std::filesystem::path keptRekordboxAnalysisFile(const std::filesystem::path &stickRoot, const std::string &pathKey);

// The entry's text before compression, and back. parse() gives Damaged
// with `error` for any text that is not this format to the letter.
std::string serializeKeptRekordboxAnalysis(const KeptRekordboxAnalysis &entry);
KeptRekordboxAnalysisLookup parseKeptRekordboxAnalysis(const std::string &text);

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
    void keep(const KeptRekordboxAnalysis &entry);
    // Nothing kept for the key is not an error.
    void drop(const std::string &pathKey);

private:
    std::filesystem::path m_stickRoot;
    std::function<void(const std::string &)> m_beforeChange;
};

// The entry's first five lines, read without the kept bytes after them
// (and so without its checksum, which covers the whole file): what a
// listing and the question "is this grid Seabass's" need. nullopt for a
// file whose head is not this format.
struct KeptRekordboxAnalysisHead
{
    std::string stickPath;
    std::string pathKey;
    std::string originalGrid;
    std::vector<std::string> seabassGrids;
};
std::optional<KeptRekordboxAnalysisHead> readKeptRekordboxAnalysisHead(const std::filesystem::path &file);

// Every grid any entry on the stick records as Seabass's (the heads'
// seabassGrids), for a decision about a track whose own entry and ledger
// entry say nothing (a track moved with neither carried). An entry whose
// head cannot be read adds none. `error` says when the folder cannot be
// read whole (required).
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
};

// Every entry in the stick's two kept-analysis folders. A folder that is
// not there is an empty list; one that cannot be read gives what was read
// and `error` saying why (required).
std::vector<KeptAnalysisFileInfo> listKeptRekordboxAnalyses(const std::filesystem::path &stickRoot, std::string *error);
std::vector<KeptAnalysisFileInfo> listKeptEngineAnalyses(const std::filesystem::path &stickRoot, std::string *error);

}  // namespace seabass::infrastructure::local
