// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "domain/beat_grid.hpp"

namespace seabass::application
{

// What BeatGridWriter::writeBeatGrid() does with a row the player has not analysed.
// Such a row has no sample rate and no length of its own, and what a
// player makes of a grid on it is not known yet: Fill and FillAndLock
// are there to find out on hardware, Skip is the default.
enum class UnanalysedRowGrid { Skip, Fill, FillAndLock };

struct BeatGridWriteOptions
{
    UnanalysedRowGrid unanalysed = UnanalysedRowGrid::Skip;
    // Write over a grid the user set on the player: a row whose grid
    // is locked, or whose adjusted grid differs from Engine's own.
    bool overwriteEngineEdits = false;
    // The audio file's rate and length, for a row with no beat data
    // of its own (Fill and FillAndLock need both).
    std::optional<double> fileSampleRateHz;
    std::optional<double> fileDurationMs;
    // The fingerprint (BeatGridWriteResult::gridFingerprint) of the grid
    // Seabass itself last wrote to this row. A row whose adjusted grid
    // still has it was not edited on the player, only by an earlier
    // conversion, so the check for player edits does not refuse it. A
    // locked row is still refused.
    std::optional<std::string> ownGridFingerprint;
    // Every check runs and the result is the one the same call would
    // return without this, fingerprint included (of the grid it would
    // leave), but nothing is written: no grid, no lock, and the hook
    // before a write is not called. For a page that says what a copy
    // would do before the user asks for it.
    bool previewOnly = false;
    // As AnalysisImportOptions::keepKey.
    std::string keepKey;
};

struct BeatGridWriteResult
{
    enum class Outcome {
        Written,
        // The row's grid is the one given, to within half a sample:
        // nothing written, so the row's edit time stays.
        AlreadySame,
        // Fewer than two usable beats: no tempo to write.
        NoSourceGrid,
        // The player has not analysed the row and the options say Skip.
        RowNotAnalysed,
        RowLocked,
        RowEditedOnEngine,
        // Fill or FillAndLock without the file's rate and length.
        NoSampleRate,
        // Engine 1.x, whose beat grids this does not write.
        UnsupportedSchema,
        // The caller asked for no grid (AnalysisImportOptions::grids off);
        // writeBeatGrid() itself never gives it.
        NotRequested,
    };
    Outcome outcome = Outcome::NoSourceGrid;
    int markers = 0;
    int barBreaks = 0;
    // For Written and AlreadySame, a SHA-256 in hex of the row's adjusted
    // grid as it is after the call: each marker's beat number and sample
    // offset to the whole sample, with the row's rate and length. Empty
    // for every other outcome. Kept by the caller, it says on a later
    // call whether the grid there is still the one written here.
    std::string gridFingerprint;
};

// Puts a rekordbox beat grid into one Engine row. The Engine writer
// (infrastructure/engine/libdjinterop_engine_cue_writer.hpp) is the one
// there is; Copy Beat Grids (use_cases/copy_beat_grids.hpp) takes it as
// this, so a test can hand it a fake.
class BeatGridWriter
{
public:
    virtual ~BeatGridWriter() = default;
    // Every refusal is an outcome with nothing written; an error the row
    // cannot be read or written past throws.
    virtual BeatGridWriteResult writeBeatGrid(const std::string &engineTrackId,
                                              const std::vector<domain::Beat> &rekordboxBeats,
                                              const BeatGridWriteOptions &options) = 0;
};

}  // namespace seabass::application
