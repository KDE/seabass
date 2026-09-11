#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/library_reader.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"

struct sqlite3;

namespace seabass::infrastructure::local
{

// Where a batch of tracks came from, so a stored row can say which stick
// it was last seen on.
struct MetadataSource
{
    std::filesystem::path stickRoot;  // to reduce absolute paths to stick-relative ones
    std::string libraryId;            // application::StickIdentity::libraryId()
    std::string stickLabel;           // for display only
    // When this stick's catalogs were last written, in seconds since the
    // epoch, 0 when it could not be read. The merge rule's last step
    // needs a date for the incoming side, and a catalog file's mtime is
    // the only one a stick offers: cues live in the catalog databases,
    // so editing a cue touches that file and nothing else. Per-stick
    // rather than per-track, because nothing finer than that exists.
    std::int64_t catalogModifiedAt = 0;
};

struct MetadataBackupSummary
{
    int tracksSeen = 0;
    int tracksAdded = 0;
    int tracksUpdated = 0;
    // At least one authored field differed and the merge rule kept the
    // stored copy. Counted per track, not per field.
    int tracksSkipped = 0;
    // Already stored and nothing incoming was new.
    int tracksUnchanged = 0;
    // Nothing to recognise the row by later: no artist and title, and
    // no filename either, or a streaming link that names no file on any
    // stick. Not an error, and counted separately so the totals add up.
    int tracksWithoutIdentity = 0;
    int cuesStored = 0;
    int artworkFilesAdded = 0;
    std::uint64_t artworkBytesAdded = 0;
    bool cancelled = false;
};

// One row as the browse view needs it: enough to render a line without a
// second query, and no cues (those are fetched for the one row a user
// opens, not for the page they scroll past).
struct StoredTrack
{
    std::int64_t id = 0;
    std::string relativePath;
    std::string filename;
    std::string title;
    std::string artist;
    double durationSeconds = 0.0;
    double bpm = 0.0;
    std::string key;
    std::optional<int> rating;
    std::string comment;
    std::optional<int> playCount;
    std::string lastPlayedAt;
    // Absolute path to the copied cover image, empty when there is none.
    std::string artworkPath;
    std::string stickLabel;
    std::string sourceFormat;
    std::string updatedAt;
    int cueCount = 0;
    int playlistCount = 0;
};

// The DJ's own work, kept on this computer instead of only on a stick
// that gets reformatted. See docs/metadata-backup-plan.md for what is
// stored and why, and for the matching rules.
//
// Backed by SQLite, which libdjinterop already requires, so this adds no
// dependency. Cover art is not in the database: images are copied to
// artworkDir() under their own SHA-256, and the row keeps the hash. A
// library where 1500 tracks share 300 covers stores 300 files, and a
// re-run never writes an image twice.
//
// Implements LibraryReader so the store can be matched against a stick
// with the same domain::matchTracks() used for rekordbox<->Engine sync.
// One matcher, not two.
class MetadataStore : public application::LibraryReader
{
public:
    // Defaults to ~/Seabass/metadata/metadata.db, creating the schema on
    // first use. An explicit path is accepted for tests, and artwork then
    // lives in an "artwork" directory beside it.
    explicit MetadataStore(std::filesystem::path databasePath = defaultDatabasePath());
    ~MetadataStore() override;

    MetadataStore(const MetadataStore &) = delete;
    MetadataStore &operator=(const MetadataStore &) = delete;

    // Every stored track, as domain tracks, for matching. format is
    // "metadata-store" and sourceId is the row id as text, so a caller
    // can get back to the row a match came from.
    std::vector<domain::Track> readAll() override;

    // Reads `tracks` into the store. Only ever writes here, never to the
    // stick they came from.
    //
    // Where a track is already stored and the two copies disagree, the
    // shared merge rule decides per field (domain::metadata_merge.hpp):
    // a blank is filled, more cues wins, otherwise the later edit wins.
    // There is no policy argument, because there is no longer a question
    // to put to the user.
    //
    // Tracks with no authored data at all (no cues, no rating, no
    // comment, no play count) are still stored: the browse view is a
    // record of what was on the stick, and a track with nothing on it
    // today may be worth annotating tomorrow. What is skipped is a track
    // with no resolvable file path, which has no key to be found again by.
    MetadataBackupSummary store(const std::vector<domain::Track> &tracks, const MetadataSource &source,
                                 application::ProgressReporter &progress,
                                 const application::CancellationToken &cancel);

    // Removes stored tracks, and the cues, playlist rows and artwork
    // references that hang off them, by row id. Returns how many rows
    // actually went.
    //
    // The copied cover images are deliberately left on disk: several
    // rows can share one file under its hash, so deleting alongside a
    // row would need a reference count this store does not keep, and an
    // orphaned image costs disk space while a wrongly deleted one costs
    // a cover on a track that still exists.
    int removeTracks(const std::vector<std::int64_t> &trackIds);

    // ---- browse -------------------------------------------------------
    // `search` matches title, artist or filename, case-insensitively;
    // empty matches everything. Ordered by artist then title. Paged in
    // SQL rather than in the caller, so showing twenty rows costs twenty
    // rows.
    std::vector<StoredTrack> browse(const std::string &search, int limit, int offset);
    int trackCount(const std::string &search = "");
    std::vector<domain::CuePoint> cuesFor(std::int64_t trackId);
    std::vector<domain::PlaylistMembership> playlistsFor(std::int64_t trackId);

    // Which stick each stored row was last seen on, by row id.
    //
    // A separate query rather than a field on the Track readAll()
    // returns: "last seen on RV2" is something to show a person, not
    // something any matching or merging decision may turn on, and
    // domain::Track is the type those decisions are made against.
    // Keeping it out of there is what stops it being used for one.
    std::map<std::int64_t, std::string> stickLabelsByTrackId();

    // Total bytes of copied cover art, for the page that has to say what
    // this feature costs on disk.
    std::uint64_t artworkBytesOnDisk() const;

    static std::filesystem::path defaultDatabasePath();
    // Where cover art copies live, derived from the database path.
    std::filesystem::path artworkDir() const;

private:
    void openAndMigrate();

    std::filesystem::path m_databasePath;
    sqlite3 *m_db = nullptr;
};

}  // namespace seabass::infrastructure::local
