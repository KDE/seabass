// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "application/ports/kept_engine_analysis.hpp"

// The player's own analysis of each Engine row Seabass replaced, on the
// stick: <stick>/Seabass/engine-analysis/<name>.tsv
// (paths::stickKeptEngineAnalysisDir), one file per track. <name> is the
// first 32 hex digits of the SHA-256 of the track's ledger key: the key is
// a whole path, too long and too free to be a FAT name.
//
// One file per track rather than one container: an entry is 4 to 8 kB
// (the overview column and the overview file are 2 to 4 kB each, the rest
// a few hundred bytes; base64 adds a third), so 5000 tracks are 20 to 40
// MB. One container would be rewritten whole, and backed up whole by every
// save, for each track kept; a file per track is written once, when the
// track is first replaced, and a save backs up only the files it touches.
// The cost is the filesystem's: a small file takes a whole cluster (32 kB
// on a FAT32 stick, up to 128 kB on a large exFAT one), so 5000 entries
// can take 160 to 640 MB of space for 20 to 40 MB of data.
//
// Format version 1: plain text, one field a line, in this order:
//
//   seabass-engine-analysis<TAB>1
//   pathKey<TAB>key                (escaped as BackupManifest escapes)
//   trackFile<TAB>Track.path       (escaped)
//   trackId<TAB>decimal
//   library<TAB>Information.uuid   (escaped)
//   seabassState<TAB>64 hex | -
//   seabassAnalysis<TAB>64 hex | -
//   isAnalyzed<TAB>value
//   isBeatGridLocked<TAB>value
//   bpm<TAB>value
//   bpmAnalyzed<TAB>value
//   key<TAB>value
//   trackData<TAB>value
//   overviewWaveFormData<TAB>value
//   beatData<TAB>value
//   overviewFilePath<TAB>path      (escaped)
//   overviewFile<TAB>none | blob:base64
//   #sha256<TAB>hex of every byte above
//
// A value is a SQLite value as stored: "null", "int:" and a decimal,
// "real:" and the 16 hex digits of the IEEE double's bits (exact),
// "text:" or "blob:" and the bytes in base64. Every line ends with a
// newline. A reader that meets any other version, a field out of order,
// a bad value or a checksum that does not match refuses the file: the
// entry is Damaged and nothing in it is believed.
namespace seabass::infrastructure::local
{

inline constexpr int KeptEngineAnalysisFormatVersion = 1;

// The file of a track's entry.
std::filesystem::path keptEngineAnalysisFile(const std::filesystem::path &stickRoot, const std::string &pathKey);

// The file's text, and back. parse() gives Damaged with `error` for any
// text that is not this format to the letter.
std::string serializeKeptEngineAnalysis(const application::KeptEngineAnalysis &entry);
application::KeptEngineAnalysisLookup parseKeptEngineAnalysis(const std::string &text);

// The store on one stick. Before a file is created, replaced or removed,
// `beforeChange` is called with its path (UTF-8): a GUI save backs it up
// and protects it for the change there, the command line backs it up. If
// it throws, the file is not touched and the exception leaves the call.
class StickKeptEngineAnalysisStore : public application::KeptEngineAnalysisStore
{
public:
    explicit StickKeptEngineAnalysisStore(std::filesystem::path stickRoot) : m_stickRoot(std::move(stickRoot)) {}

    void setBeforeChange(std::function<void(const std::string &file)> hook) { m_beforeChange = std::move(hook); }

    application::KeptEngineAnalysisLookup find(const std::string &pathKey) override;
    void keep(const application::KeptEngineAnalysis &entry) override;
    void drop(const std::string &pathKey) override;

private:
    std::filesystem::path m_stickRoot;
    std::function<void(const std::string &)> m_beforeChange;
};

}  // namespace seabass::infrastructure::local
