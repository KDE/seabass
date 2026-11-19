// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The player's own analysis of one Engine row, kept on the stick before
// Seabass replaces any of it, so that it can be put back byte for byte
// (Beat Grids & Waveforms, "Restore Engine's Analysis").
namespace seabass::application
{

// One column as SQLite holds it: its storage class and its value, so a
// value goes back exactly as it was (an INTEGER stays one, a NULL stays
// NULL, a blob keeps every byte).
struct EngineSqlValue
{
    enum class Type { Null, Integer, Real, Text, Blob };
    Type type = Type::Null;
    std::int64_t integer = 0;
    double real = 0.0;
    // Text or blob bytes.
    std::string bytes;

    static EngineSqlValue null() { return {}; }
    static EngineSqlValue ofInteger(std::int64_t v) { return {Type::Integer, v, 0.0, {}}; }
    static EngineSqlValue ofReal(double v) { return {Type::Real, 0, v, {}}; }
    static EngineSqlValue ofBlob(std::string b) { return {Type::Blob, 0, 0.0, std::move(b)}; }
    static EngineSqlValue ofText(std::string t) { return {Type::Text, 0, 0.0, std::move(t)}; }
    bool isNull() const { return type == Type::Null; }

    friend bool operator==(const EngineSqlValue &, const EngineSqlValue &) = default;
};

// The analysis of one Engine row: the three blobs as stored (compressed,
// as the player wrote them), the flags and numbers beside them, and the
// overview file in Database2/OverviewData. Engine 3.x keeps the blobs in
// PerformanceData and the rest in Track; 2.x all of it in Track.
struct EngineAnalysisColumns
{
    EngineSqlValue isAnalyzed;
    EngineSqlValue isBeatGridLocked;
    EngineSqlValue bpm;
    EngineSqlValue bpmAnalyzed;
    EngineSqlValue key;
    EngineSqlValue trackData;
    EngineSqlValue overviewWaveFormData;
    EngineSqlValue beatData;
    // The overview file's bytes; nullopt when there was none.
    std::optional<std::string> overviewFile;
    // Its path from the Engine Library folder, '/'-separated
    // ("Database2/OverviewData/<uuid>/<id>.rgb").
    std::string overviewFileRelativePath;

    friend bool operator==(const EngineAnalysisColumns &, const EngineAnalysisColumns &) = default;
};

// What the store keeps for one track.
struct KeptEngineAnalysis
{
    // The ledger's key for the track (normalizedPathKey of its path from
    // the stick root): never an Engine row id, which Engine hands on.
    std::string pathKey;
    // What the entry belongs to, to see that it is stale: the row's
    // Track.path as stored, its id and the library's Information.uuid.
    std::string trackFile;
    std::int64_t trackId = 0;
    std::string libraryUuid;
    // The player's analysis, as it was before Seabass replaced it.
    EngineAnalysisColumns original;
    // The row as Seabass last left it, as two fingerprints (64 lowercase
    // hex digits, empty: not known): of the whole row (every column above
    // and the overview file), and of the part only an analysis writes
    // (the analysis record, the overview, Engine's own grid with its rate
    // and length, the analysed flag, the overview file). A DJ's edit on the
    // player changes the first and not the second; the player's new
    // analysis changes both.
    std::string seabassState;
    std::string seabassAnalysis;
    // The row in its two parts, before and after Seabass's last write to
    // it: the columns (as the whole row, and as only the analysis), and the
    // overview file, each a fingerprint, the after first. A write is cut
    // short between its parts (the columns written, the file not), so a row
    // whose every part is one of these, or the original's, is Seabass's
    // own, finished or not, and never taken for the player's new analysis.
    // Empty in an entry of format 1, which did not record them.
    std::vector<std::string> seabassColumnStates;
    std::vector<std::string> seabassColumnAnalyses;
    std::vector<std::string> seabassFiles;

    friend bool operator==(const KeptEngineAnalysis &, const KeptEngineAnalysis &) = default;
};

// What looking a track up found.
struct KeptEngineAnalysisLookup
{
    enum class Status {
        None,
        Found,
        // There is a file for the track that cannot be believed: cut
        // short, another version, a bad checksum. `error` says why.
        Damaged,
    };
    Status status = Status::None;
    KeptEngineAnalysis entry;
    std::string error;
};

// Where the kept analyses live (infrastructure/local/kept_engine_analysis_file.hpp
// on the stick). Every write is durable; a failure throws, with nothing
// changed.
class KeptEngineAnalysisStore
{
public:
    virtual ~KeptEngineAnalysisStore() = default;
    virtual KeptEngineAnalysisLookup find(const std::string &pathKey) = 0;
    virtual void keep(const KeptEngineAnalysis &entry) = 0;
    // Nothing kept for the key is not an error.
    virtual void drop(const std::string &pathKey) = 0;
};

}  // namespace seabass::application
