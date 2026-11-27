// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/beat_grid_writer.hpp"
#include "application/ports/kept_engine_analysis.hpp"
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
    // hand.
    bool overwriteEngineEdits = false;
    // Write over the waveform the player drew of a row it analysed. Off, a
    // row's waveform is written only where it has none, or where it is one
    // Seabass wrote (PlayersWaveform otherwise). Seabass's own is known as
    // the kept store knows it (the player's original kept, and the row's
    // analysis one Seabass left there) or, with no kept entry, as the
    // keeping knows it: a row Seabass completed, both its grids the
    // ledger's.
    bool overwritePlayerWaveforms = false;
    // A grid and a waveform come from one analysis: where the player's
    // grid stays (locked or edited on the player, unless a call that
    // writes grids has overwriteEngineEdits; or rekordbox has none), its
    // waveform stays with it, PlayersWaveform, whatever
    // overwritePlayerWaveforms says, and whether grids are asked for or
    // not (AnalysisImportResult::waveformStaysWithGrid). The Beat Grids
    // page's rule.
    bool waveformFollowsKeptGrid = false;
    // A sync's rule (#136): where both sides hold a grid, or both a
    // waveform, and they differ, neither is picked. The part is left alone
    // and said to differ (DiffersFromEngine) instead of being written over
    // the player's own, or left as RowLocked, RowEditedOnEngine or
    // PlayersWaveform: the user answers it as a conflict. A grid Seabass
    // wrote there (the ledger's) or a waveform of an analysis Seabass knows
    // is not Engine's own and is replaced as before; an empty side (no
    // grid, no waveform) is never a side of a difference.
    // overwriteEngineEdits and overwritePlayerWaveforms, an answer's, take
    // precedence for their part.
    bool differingIsConflict = false;
    // With overwriteEngineEdits: a grid locked on the player still stays
    // (RowLocked). For an answer given while the grid was not locked: a
    // lock set on the player since is the DJ's newer word.
    bool respectLock = false;
    // As BeatGridWriteOptions::ownGridFingerprint.
    std::optional<std::string> ownGridFingerprint;
    // The audio file's sample rate, for a row with no analysis record of
    // its own, and whether it is an MP3 (the player counts whole frames).
    std::optional<double> fileSampleRateHz;
    bool fileIsMp3 = false;
    // The file's own sample count, read from its container where that is
    // exact (infrastructure/audio/exact_sample_count.hpp: FLAC, WAV, AIFF,
    // M4A, and an MP3's frames), and whether the file is of a kind whose
    // count must come from it (FLAC, WAV, AIFF, M4A): such a file without
    // its count is not completed from an estimate (NoSampleCount). Measured
    // on a Prime 4: what it stores is the container's count to the sample
    // for FLAC, AIFF and M4A, the frames times 1152 for an MP3. Where both
    // are there, the file's count wins over rekordbox's scrolling waveform.
    std::optional<std::uint64_t> fileSampleCount;
    bool fileCountRequired = false;
    // The track's ledger key (BeatGridCopyItem::ledgerKey): where the
    // player's own analysis of the row is kept before Seabass replaces any
    // of it, when the writer has a KeptEngineAnalysisStore.
    std::string keepKey;
    // On a row the player has analysed, the tempo (analysis.bpm) goes in
    // with the grid, in the same write, when the grid part is written or
    // is that grid already: for a grid that is Seabass's own analysis of
    // the file (Analyse again), whose tempo the old one would contradict.
    // Off, as for rekordbox's grid, the tempo stays what the row has.
    // Never beside a grid left alone (locked, moved on the player): that
    // grid keeps its tempo. A row being completed takes the tempo either
    // way.
    bool tempoWithGrid = false;
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
        // As BeatGridWriteResult's: the player's waveform would have to be
        // kept first and cannot be, or the row's flags cannot be read.
        CannotKeepEngines,
        RowUnreadable,
        // The player drew its own waveform of the row, and
        // overwritePlayerWaveforms is off: left as it is.
        PlayersWaveform,
        // As PlayersWaveform, under differingIsConflict: the player's
        // waveform and rekordbox's differ, and neither is picked.
        DiffersFromEngine,
    };
    Waveform waveform = Waveform::NotRequested;

    // A row the player had not analysed was given everything it needs and
    // marked analysed (in a preview: would be).
    bool completedRow = false;

    // The row's flag says analysed, but it holds no analysis at all: no
    // beat data, overview or analysis record, and no overview file (Engine
    // DJ 5.1 left rows so). Completed like one the player has not analysed,
    // with both parts or not at all; nothing of the player's to keep.
    bool analysedNothingStored = false;

    // The row's grid is locked on the player (a lock that cannot be read
    // counts as one), as the call found it.
    bool rowLocked = false;

    // The row's grid was moved on the player: its adjusted grid is not
    // Engine's analysis and not the one Seabass wrote last. Found whatever
    // the options say, so a page can tell hand-edited rows apart.
    bool rowEditedOnEngine = false;

    // The player's waveform was left because the player's grid stays
    // (waveformFollowsKeptGrid): rowLocked, rowEditedOnEngine, or neither,
    // which is rekordbox having no grid for the track.
    bool waveformStaysWithGrid = false;

    // The tempo went in with the grid (AnalysisImportOptions::
    // tempoWithGrid; in a preview: would). A completed row takes it as
    // part of completedRow.
    bool tempoWritten = false;

    // The overview file's path from the Engine Library folder, '/'-separated,
    // when one was (in a preview: would be) written.
    std::string overviewFileRelativePath;

    // NoSampleCount: why the row's length is not known, in plain words.
    std::string noSampleCountReason;

    // The player's own analysis of the row was kept on the stick before
    // this write replaced it: the first time, or again because the player
    // had analysed the row anew since. Never in a preview.
    bool keptEngineAnalysis = false;
};

// Putting the player's own analysis back (KeptEngineAnalysis).
struct EngineAnalysisRestoreOptions
{
    // Also over a row whose grid, key or tempo was changed on the player
    // since Seabass wrote it. Never over the player's new analysis, which
    // is newer than what was kept and would be lost unkept (ChangedSince
    // all the same).
    bool force = false;
    bool previewOnly = false;
};

struct EngineAnalysisRestoreResult
{
    enum class Outcome {
        // Put back, the kept entry dropped (in a preview: would be).
        Restored,
        // The row holds the kept analysis already: the entry is dropped,
        // nothing else written.
        AlreadyEngines,
        // Nothing is kept for the track.
        NothingKept,
        // The kept entry cannot be read.
        KeptDamaged,
        // The kept entry is of another row, file or library.
        Stale,
        // The row changed since Seabass wrote it (the player analysed it
        // again, or the DJ moved or locked the grid): newer work, left
        // alone; a grid, key or tempo changed is written over when forced,
        // the player's new analysis never.
        ChangedSince,
        UnsupportedSchema,
    };
    Outcome outcome = Outcome::NothingKept;
    std::string reason;
    // The overview file put back or taken away, from the Engine Library
    // folder; empty when it was left as it is.
    std::string overviewFileRelativePath;
};

struct LetPlayerAnalyseResult
{
    enum class Outcome {
        // Marked for the player's analysis (in a preview: would be).
        Marked,
        // The row is not analysed: nothing to do.
        NotAnalysed,
        UnsupportedSchema,
    };
    Outcome outcome = Outcome::NotAnalysed;
    // What was kept of the row before could not be read and was cleared
    // (in a preview: would be), whatever the outcome: on a row whose flag
    // is down, the only thing done.
    bool clearedDamagedEntry = false;
};

// What a row's analysis is, for the page: whether the player (or Seabass)
// has analysed it, and whether the player's own analysis is kept.
struct EngineAnalysisFacts
{
    bool rowAnalysed = false;
    enum class Kept { None, Kept, Stale, Damaged };
    Kept kept = Kept::None;
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

    // The player's own analysis kept for `keepKey`, back into the row byte
    // for byte (columns and overview file, or no file where there was
    // none), then the kept entry dropped. The ledger line is the caller's.
    virtual EngineAnalysisRestoreResult restoreEngineAnalysis(const std::string &engineTrackId,
                                                              const std::string &keepKey,
                                                              const EngineAnalysisRestoreOptions &options) = 0;

    // The row marked for the player's analysis (markForDeviceAnalysis),
    // keeping what is kept of it. A damaged kept entry, which can never be
    // put back, is dropped. The ledger line is the caller's.
    virtual LetPlayerAnalyseResult letPlayerAnalyseAgain(const std::string &engineTrackId, const std::string &keepKey,
                                                         bool previewOnly) = 0;

    virtual EngineAnalysisFacts engineAnalysisFacts(const std::string &engineTrackId, const std::string &keepKey) = 0;
};

}  // namespace seabass::application
