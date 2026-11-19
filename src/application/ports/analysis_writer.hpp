// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/beat_grid_writer.hpp"
#include "domain/beat_grid.hpp"

namespace seabass::application
{

// Everything rekordbox says about one track that a Denon player would
// otherwise work out by analysing it: the beat grid, the waveform it draws
// the overview from, the key and the tempo. Each part may be absent.
struct RekordboxAnalysis
{
    // Every beat, on rekordbox's timeline, as the rekordbox reader gives it.
    std::vector<domain::Beat> beats;
    // Raw section bodies (see infrastructure/rekordbox/rekordbox_waveform_reader.hpp):
    // the three-band preview (PWV6), the colour preview (PWV4), and how many
    // entries the scrolling waveform has (150 a second).
    std::optional<std::string> threeBandPreview;
    std::optional<std::string> colourPreview;
    std::optional<std::uint32_t> scrollEntries;
    // Engine's key integer (libdjinterop's musical_key).
    std::optional<int> key;
    std::optional<double> bpm;
};

struct AnalysisImportOptions
{
    bool grids = true;
    bool waveforms = true;
    // Write over a grid the user set on the player: locked, or moved by
    // hand. The waveform has no such state and is always replaced.
    bool overwriteEngineEdits = false;
    // As BeatGridWriteOptions::ownGridFingerprint.
    std::optional<std::string> ownGridFingerprint;
    // The audio file's sample rate, for a row with no analysis record of
    // its own, and whether it is an MP3 (the player counts whole frames).
    std::optional<double> fileSampleRateHz;
    bool fileIsMp3 = false;
    // Every check, no write, no file and no hook; the result is the one the
    // same call would give without it.
    bool previewOnly = false;
    // For testing on hardware only: leave the overview file out, or write
    // zero loudness into a completed row's analysis record.
    bool skipOverviewFile = false;
    bool zeroLoudness = false;
};

struct AnalysisImportResult
{
    // As writeBeatGrid() reports it, NotRequested when grids was off.
    BeatGridWriteResult grid;

    enum class Waveform {
        Written,
        // The converted overview, and the overview file unless it was left
        // out, are there already: nothing written.
        AlreadySame,
        NotRequested,
        // rekordbox has neither preview for the track.
        NoSourceWaveform,
        // The player has not analysed the row and it could not be completed
        // (see importAnalysis()), for a reason that is not the waveform's.
        RowNotAnalysed,
        // A row the player has not analysed, with no way to know its
        // length: no scrolling waveform entry count or no file sample rate.
        NoSampleCount,
        UnsupportedSchema,
    };
    Waveform waveform = Waveform::NotRequested;

    // A row the player had not analysed was given everything it needs and
    // marked analysed (in a preview: would be).
    bool completedRow = false;

    // The overview file's path from the Engine Library folder, '/'-separated,
    // when one was (in a preview: would be) written.
    std::string overviewFileRelativePath;
};

// Puts rekordbox's analysis into one Engine row. The Engine writer
// (infrastructure/engine/libdjinterop_engine_cue_writer.hpp) is the one there
// is; a use case takes it as this, so a test can hand it a fake.
class AnalysisWriter
{
public:
    virtual ~AnalysisWriter() = default;
    // Every refusal is an outcome with nothing written; an error the row
    // cannot be read or written past throws.
    virtual AnalysisImportResult importAnalysis(const std::string &engineTrackId, const RekordboxAnalysis &analysis,
                                                const AnalysisImportOptions &options) = 0;
};

}  // namespace seabass::application
