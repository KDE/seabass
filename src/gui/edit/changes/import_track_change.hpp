// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "application/ports/cancellation_token.hpp"
#include "domain/beat_grid.hpp"
#include "domain/imported_track.hpp"
#include "domain/waveform_from_audio.hpp"
#include "gui/edit/pending_change.hpp"
#include "infrastructure/rekordbox/anlz_builder.hpp"

namespace seabass::gui
{

// The track import: one audio file from the computer put onto the stick
// and given a row in every library the stick has. In order, inside one
// save step:
//   1. the file is copied to its place under the stick's root and
//      verified (infrastructure::track_import::copyFileVerified: never
//      over a file that is there, never from a file that changed while
//      it was read);
//   2. DeviceLibrary: a track row in export.pdb with the lookup rows it
//      needs (PdbRowWriter::addTrack);
//   3. OneLibrary: a content row in exportLibrary.db
//      (OneLibraryCueWriter::addTrack);
//   4. Engine: a Track row in m.db (createEngineTrack), left for the
//      player to analyse.
// All three rows are filled from the one domain::ImportedTrack, so the
// libraries say the same about the track.
//
// `libraries` has to name exactly the libraries the stick has: apply()
// refuses otherwise, before it writes (an import that reaches only some
// of a stick's libraries leaves them disagreeing, which is the state the
// app exists to prevent).
//
// A failed change takes the whole save back (runSaveLoop), every track
// imported before it included. So everything that can be asked before
// the copy is: what each library's writer would refuse the row for, the
// libraries on the stick, the source file. The copy itself is not
// cancellable for the same reason; a cancel waits for the file in hand.
// What is left to fail after the copy is the stick itself.
//
// The copied audio file is declared as a file this change creates, so a
// failed save and Undo Last Save both take it away with the rows. The
// folders the copy made stay, empty.
//
// The cover from the file's own tags goes with the rows (setCover).
//
// Not this change's: playlists (AddToPlaylistChange and
// EngineMembershipChange find the new rows by their file, staged after
// this one and tied to it by unitKeys()).
// A rehearsal of a batch of imports against the stick's export.pdb as it
// is now: each track's row is added to a copy of the file in memory that
// is never written, so what only the library can refuse a row for, and
// only once the rows before it are in (the last page filling up where no
// new page can be started, a lookup table that cannot take another
// name), is known when the batch is staged. A failed change takes the
// whole save back, so the one track that cannot go in must not be staged
// with the forty that can.
//
// Reads the file once, at construction; writes nothing, ever. A stick
// without export.pdb rehearses nothing and refuses nothing. The other
// two libraries have no state of this kind: what they refuse a row for
// beyond its own contents is the library as a whole (a schema, a missing
// row), which the first change of a save meets with nothing yet to lose.
class ImportRehearsal
{
public:
    // pioneerRoot: empty, or a folder without rekordbox/export.pdb, for a
    // stick without DeviceLibrary. Throws when the file is there and
    // cannot be read as one.
    explicit ImportRehearsal(const QString &pioneerRoot);
    ~ImportRehearsal();
    ImportRehearsal(const ImportRehearsal &) = delete;
    ImportRehearsal &operator=(const ImportRehearsal &) = delete;

    // The playlist the batch goes into, by its path: from here on add()
    // also rehearses the track's entry in it, as the AddToPlaylistChange
    // staged behind each import writes one. With `create`, a playlist
    // export.pdb does not have is made in the copy first, at the top
    // level, as CreatePlaylistChange makes it. Empty when the copy has
    // the playlist now; otherwise why not.
    std::string usePlaylist(const std::string &path, bool create);

    // Adds `track`'s row to the rehearsal copy, and its playlist entry
    // when a playlist is in use. Empty when they went in; otherwise why
    // not. A row whose entry could not follow it stays in the copy: the
    // table that refused it refuses every later one as well.
    // `withCover`: the track's import makes an artwork row as well
    // (ImportTrackChange::setCover).
    // `withAnalysisFile`: its row will name an analysis file
    // (ImportTrackChange::setAnalysis), which makes the row longer.
    std::string add(const domain::ImportedTrack &track, std::uint64_t fileSizeBytes, bool withCover = false,
                    bool withAnalysisFile = false);

    // The playlist entry of a track export.pdb already has, by its file
    // (absolute, under the stick `pioneerRoot` is on), as an
    // AddToPlaylistChange for it writes one. Empty when it went in, or
    // was there; otherwise why not.
    std::string enter(const QString &pioneerRoot, const std::string &file);

    // Moves the entries add() and enter() made to `toIndex` in the playlist, in their
    // order, as PlaceInPlaylistChange does once for the batch. Empty when
    // the copy took it (or there is nothing to move); otherwise why not.
    std::string place(int toIndex);

private:
    struct Rows;
    std::unique_ptr<Rows> m_rows;
};

// A track's cover as JPEGs in the sizes the libraries keep: rekordbox's
// two squares of 80 and 240 pixels, and a larger one for Engine.
struct CoverPictures
{
    std::string small;
    std::string medium;
    std::string large;
};

// Reads the picture in an audio file's tags and scales it; nullopt when
// there is none or it is no picture. Given from outside: scaling a
// picture takes Qt Gui, which this library does not link
// (gui/track_import_cover.hpp has the one the app uses).
using CoverReader = std::function<std::optional<CoverPictures>(const std::string &audioFile)>;

// What Seabass's own analysis made of an audio file (a test version: the
// grid is domain::SeabassGridAnalyser's, the waveform
// domain::waveformFromSamples').
struct ImportAnalysis
{
    // Every beat, on the decoder's timeline, which is Engine's
    // (infrastructure/audio/playback_timeline.hpp). Empty: no grid.
    std::vector<domain::Beat> beats;
    domain::AudioWaveform waveform;
    // The tempo of the grid, the median of its beat spacings; 0 without
    // one. Not rounded.
    double bpm = 0.0;
    // As the analyser gives it, 0 to 1.
    double confidence = 0.0;
    // The grid follows a tempo that moves (GridAnalysis::followsTempo):
    // one to look at before it is trusted.
    bool followsTempo = false;
    // The key the analyser heard, as rekordbox spells it ("F#m"); empty
    // where it heard none. Goes into the rows of a file whose tags name
    // no key: a tag is somebody's choice, the analysis is right on about
    // two tracks in three.
    std::string key;

    // What made the analysis, and of which file, for the stick's analysis
    // ledger (infrastructure/local/analysis_ledger_file.hpp).
    struct Provenance
    {
        // infrastructure::audio::AnalysisInputRevision.
        int inputRevision = 0;
        // The result's musicanalyze::Revisions.
        int gridRevision = 0;
        int keyRevision = 0;
        int waveformRevision = 0;
        // musicanalyze::version().
        std::string libraryVersion;
        // The tempo range the analysis ran with (musicanalyze::
        // usableTempoRange() of the one asked for), as
        // formatTempoBpm() writes it.
        std::string tempoMin;
        std::string tempoMax;
        // The file analysed, the same before and after it was decoded.
        std::uint64_t fileSize = 0;
        std::int64_t fileMtimeUnix = 0;
    };
    // Set by analyseForImport() for a file decoded whole that did not
    // change while it was read. An analysis without it is written to the
    // libraries all the same and gets no ledger entry: a record that
    // cannot say what made it would be believed for the wrong revisions.
    std::optional<Provenance> provenance;
};

// Decodes and analyses a file; null when it cannot be decoded. Given from
// outside: decoding takes Qt Multimedia, which this library does not
// link (gui/track_import_analysis.hpp has the one the app uses). Throws
// application::OperationCancelled on a cancel.
using ImportAnalyser = std::function<std::shared_ptr<const ImportAnalysis>(const std::string &audioFile,
                                                                         const application::CancellationToken &cancel)>;

// An analysis as rekordbox's three analysis files of a track, as the
// import writes them and Analyse again merges them into a track's own:
// the grid moved onto rekordbox's timeline (an MP3's lead-in, read from
// `audioFile`) and cut to the track, the waveform moved with it by whole
// entries, the track's length kept.
struct RekordboxAnalysisFiles
{
    // None when the analysis has no waveform, or the builder refused it
    // (`error` says why).
    std::optional<infrastructure::rekordbox::NewAnlzFiles> files;
    std::string error;
    // The grid on rekordbox's timeline; empty without one, and for an MP3
    // whose lead-in cannot be read, which has no known place on it.
    std::vector<domain::Beat> beats;
    // The file's own sample count where its container says it exactly.
    std::optional<std::uint64_t> sampleCount;
    bool isMp3 = false;
};
// `stickPath`: the file's place on the stick, as PPTH spells it;
// `durationSeconds`: its length where the waveform does not say.
RekordboxAnalysisFiles rekordboxAnalysisFilesFor(const ImportAnalysis &analysis, const std::string &audioFile,
                                                 const std::string &stickPath, double durationSeconds);

class ImportTrackChange : public PendingChange
{
public:
    // `sourceFile`: the file on the computer, as the bytes of its path (a
    // name that is not UTF-8 does not survive a QString). `stickRoot`: the stick as it
    // is mounted now; `track.stickPath` is the file's place under it.
    // `pioneerRoot` and `enginePath` as every change takes them, empty for
    // a library the stick does not have. `libraries`: "rekordbox",
    // "onelibrary", "engine".
    ImportTrackChange(std::string sourceFile, QString stickRoot, QString pioneerRoot, QString enginePath,
                      domain::ImportedTrack track, QStringList libraries, int itemCountHint);

    // The source file has a picture in its tags: apply() reads it with
    // `reader` and gives the track that cover in every library. For
    // rekordbox's two the pictures go under /PIONEER/Artwork/ by the id
    // of the artwork row made for them (aN.jpg and aN_m.jpg for
    // export.pdb, bN.jpg and bN_m.jpg for OneLibrary); for Engine the
    // large one goes into the library's own storage. One cover per
    // track: two tracks of one album each get their own files.
    //
    // A picture that cannot be read by then, or whose file name on the
    // stick is taken, is no cover and no failure. Without a reader the
    // track gets none.
    void setCover(CoverReader reader) { m_coverReader = std::move(reader); }
    bool withCover() const { return static_cast<bool>(m_coverReader); }

    // The file's analysis, made before the save: apply() then writes
    //   - for rekordbox's libraries, new analysis files (.DAT, .EXT,
    //     .2EX, infrastructure::rekordbox::buildAnlzFiles) in a free
    //     folder under /PIONEER/USBANLZ/, named in both rows, with the
    //     beat grid moved onto rekordbox's timeline (an MP3's lead-in)
    //     and no cues;
    //   - for Engine, grid and overview waveform into the new row through
    //     the analysis writer the beat grid copy uses, which marks the
    //     row analysed.
    // The rows' tempo is then the grid's, not the tag's. A track
    // without a grid (none found, or an MP3 whose lead-in cannot be
    // read, which has no known place on rekordbox's timeline) gets
    // waveforms in rekordbox's files and nothing in Engine, whose row
    // stays for the player to analyse. With the MP3 timing correction
    // switched off in Preferences the grid of an MP3 is written as
    // decoded, which is Engine's timeline and not rekordbox's. Without
    // an analysis the track goes in unanalysed, as before.
    //
    // A folder whose files are on the stick is never written into; with
    // none found free, or a USBANLZ folder that cannot be read to its
    // end, the track gets no analysis files and the log says so.
    //
    // An analysis that went into a library, and says what made it
    // (ImportAnalysis::provenance), is recorded in the stick's analysis
    // ledger once the save has committed (analysis_ledger_record.hpp),
    // with the grid each library holds afterwards.
    void setAnalysis(std::shared_ptr<const ImportAnalysis> analysis);
    bool analysed() const { return static_cast<bool>(m_analysis); }
    const std::shared_ptr<const ImportAnalysis> &analysis() const { return m_analysis; }

    // Why the libraries named would refuse a row for `track`, a file of
    // `fileSizeBytes`, for what it is; empty when none would. What
    // apply() asks before it copies, for a caller that plans: a file
    // refused here is left out of the save rather than failing it.
    // `withAnalysisFile`: asked for a row that names an analysis file.
    static std::string rowRefusal(const domain::ImportedTrack &track, const QStringList &libraries,
                                  std::uint64_t fileSizeBytes, bool withAnalysisFile = false);

    static QString idFor(const std::string &stickPath);
    // What ties the playlist changes of this track to it.
    static QString unitKeyFor(const std::string &stickPath);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList unitKeys() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

    // The file on the stick: where it is, or will be.
    std::string destinationFile() const;
    // After an apply() that succeeded: the ids of the new rows, 0 or -1
    // for a library that was not written. They name rows only while the
    // save that made them stands.
    std::uint32_t deviceLibraryId() const { return m_deviceLibraryId; }
    std::int64_t oneLibraryId() const { return m_oneLibraryId; }
    std::int64_t engineId() const { return m_engineId; }
    // After an apply() with an analysis: whether Engine's row took it and
    // is marked analysed.
    bool engineAnalysed() const { return m_engineAnalysed; }

private:
    std::string m_sourceFile;
    QString m_stickRoot;
    QString m_pioneerRoot;
    QString m_enginePath;
    domain::ImportedTrack m_track;
    QStringList m_libraries;
    int m_itemCountHint = 1;
    CoverReader m_coverReader;
    std::shared_ptr<const ImportAnalysis> m_analysis;
    std::uint32_t m_deviceLibraryId = 0;
    std::int64_t m_oneLibraryId = -1;
    std::int64_t m_engineId = -1;
    bool m_engineAnalysed = false;
    // The track's key is the analyser's: its tags named none.
    bool m_keyFromAnalysis = false;
    // The stick root the analysis ledger is kept under, as Analyse again
    // finds it.
    std::string ledgerStickRoot() const;
};

}  // namespace seabass::gui
