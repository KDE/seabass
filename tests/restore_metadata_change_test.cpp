// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// RestoreMetadataChange: the write half of Restore Metadata, driven
// against a real (SQLCipher-encrypted) OneLibrary database rather than a
// mock, because the thing worth pinning here is what actually lands in
// the file.
//
// Case 1 is a regression guard with a data-loss bug behind it. A cue
// writer takes the vector it is handed as the complete set for the
// track, so an empty one deletes every cue rather than leaving them
// alone -- and a proposal staged for its rating alone carries no cues,
// as does one whose cues conflict under the skip-all default. Writing
// unconditionally therefore wiped exactly the live work that default
// exists to protect.
//
// The rest pin the two things the format itself decides and no reader
// reads back: that OneLibrary stores a rating in stars (0 to 5) and not
// on Engine's 0-to-100 scale, and that the comment column is djComment.

#include <QString>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/metadata_restore.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/restore_metadata_change.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/work_counters.hpp"

#include "scratch_path.hpp"

using namespace seabass::gui;
using namespace seabass::domain;
using namespace seabass::infrastructure::onelibrary;
using seabass::application::CancellationToken;
namespace fs = std::filesystem;

namespace
{

// The same minimal-but-real fixture onelibrary_cue_writer_test.cpp
// builds -- see that file for why this schema shape -- plus the two
// annotation columns, which a real stick's content table also carries.
void createFixture(const std::string &pioneerRoot)
{
    fs::create_directories(fs::path(pioneerRoot) / "rekordbox");
    const std::string dbPath = OneLibraryCueWriter::dbPathFor(pioneerRoot);

    const std::string key = deriveOneLibraryKey();
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, dbPath, /*readOnly=*/false);
    db.exec("PRAGMA key = '" + key + "';");
    db.exec(
        "CREATE TABLE content(content_id integer primary key, title varchar, path varchar, "
        "rating integer, djComment varchar);");
    db.exec(
        "CREATE TABLE cue(cue_id integer primary key, content_id integer, kind integer, "
        "colorTableIndex integer, cueComment varchar, isActiveLoop integer, inUsec integer, "
        "outUsec integer);");
    db.exec("CREATE TABLE hotCueBankList_cue(hotCueBankList_id integer, cue_id integer, sequenceNo integer);");
    db.exec("CREATE TABLE playlist_content(content_id integer, playlist_id integer, sequenceNo integer);");
    db.exec("INSERT INTO content (content_id, title, path) VALUES (1, 'Test Track', '/Contents/Test Track.mp3');");
    // Two more, for the case that counts what a multi-track restore costs.
    db.exec("INSERT INTO content (content_id, title, path) VALUES (2, 'Second', '/Contents/Second.mp3');");
    db.exec("INSERT INTO content (content_id, title, path) VALUES (3, 'Third', '/Contents/Third.mp3');");
}

// Everything read back goes through an independent second connection, so
// a case proves what is in the file rather than what a writer returned.
int cueCount(const std::string &pioneerRoot)
{
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot), /*readOnly=*/true);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    SqlCipherStatement stmt(db, "SELECT COUNT(*) FROM cue WHERE content_id = 1");
    assert(stmt.step());
    return static_cast<int>(stmt.columnInt64(0));
}

std::optional<int> storedRating(const std::string &pioneerRoot)
{
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot), /*readOnly=*/true);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    SqlCipherStatement stmt(db, "SELECT rating FROM content WHERE content_id = 1");
    assert(stmt.step());
    if (stmt.columnIsNull(0)) {
        return std::nullopt;
    }
    return static_cast<int>(stmt.columnInt64(0));
}

std::string storedComment(const std::string &pioneerRoot)
{
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot), /*readOnly=*/true);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    SqlCipherStatement stmt(db, "SELECT djComment FROM content WHERE content_id = 1");
    assert(stmt.step());
    return stmt.columnText(0);
}

std::vector<CuePoint> liveCues()
{
    CuePoint hot1{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "the DJ's own"};
    CuePoint hot2{CuePoint::Kind::Hot, 2, 2000.0, "#00FF00", "set last night"};
    return {hot1, hot2};
}

std::vector<CuePoint> storedCues()
{
    CuePoint hot1{CuePoint::Kind::Hot, 1, 9000.0, "#0000FF", "from the backup"};
    return {hot1};
}

struct Fixture
{
    fs::path scratch;
    fs::path pioneerRoot;
    std::string filePath;
};

Fixture freshFixture(const std::string &name)
{
    Fixture fixture;
    fixture.scratch = seabass::testing::scratchRoot() / ("seabass_restore_metadata_change_" + name);
    std::error_code ec;
    fs::remove_all(fixture.scratch, ec);
    fs::create_directories(fixture.scratch);
    fixture.pioneerRoot = fixture.scratch / "PIONEER";
    createFixture(fixture.pioneerRoot.string());
    fixture.filePath = (fixture.scratch / "Contents" / "Test Track.mp3").string();
    return fixture;
}

// The proposal a plan would carry for this one track, with only the
// fields a case is about turned on.
MetadataRestoreProposal proposalFor(const Fixture &fixture)
{
    MetadataRestoreProposal proposal;
    proposal.storedId = "stored-1";
    proposal.stickTrack.sourceId = "1";
    proposal.stickTrack.filePath = fixture.filePath;
    proposal.stickTrack.title = "Test Track";
    return proposal;
}

void applyChange(const Fixture &fixture, const MetadataRestoreProposal &proposal)
{
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    CancellationToken token;
    const QString root = QString::fromStdString(fixture.pioneerRoot.string());
    SaveContext ctx(token, noProgress, {}, root, {});
    RestoreMetadataChange change("onelibrary", root, "1", proposal);
    const ChangeOutcome outcome = change.apply(ctx);
    assert(outcome.ok);
    assert(!ctx.runFinishHooks(true));
}

}  // namespace

int main()
{
    // Case 1: the regression. A proposal that offers a rating and no
    // cues must leave the cues on the stick exactly as they were. This
    // is the shape a track gets under the skip-all default when its cues
    // conflict with the store's -- cuesOffered false, cues empty -- and
    // the shape that used to hand an empty vector to the writer and
    // delete two hot cues the DJ set last night.
    {
        Fixture fixture = freshFixture("keeps_cues");
        {
            OneLibraryCueWriter writer(fixture.pioneerRoot.string());
            writer.writeCuesForPath(fixture.filePath, liveCues());
        }
        assert(cueCount(fixture.pioneerRoot.string()) == 2);

        MetadataRestoreProposal proposal = proposalFor(fixture);
        proposal.cuesConflict = true;  // the store had cues; they differ
        proposal.ratingOffered = true;
        proposal.rating = 4;
        assert(!proposal.cuesOffered);
        assert(proposal.cues.empty());

        applyChange(fixture, proposal);

        assert(cueCount(fixture.pioneerRoot.string()) == 2);
        assert(storedRating(fixture.pioneerRoot.string()) == 4);
        std::cout << "case 1: a rating-only restore leaves the track's own cues alone\n";
    }

    // Case 2: the same change does write cues when the plan offers them,
    // so case 1 is a guard and not an accidental no-op.
    {
        Fixture fixture = freshFixture("writes_cues");
        {
            OneLibraryCueWriter writer(fixture.pioneerRoot.string());
            writer.writeCuesForPath(fixture.filePath, liveCues());
        }
        assert(cueCount(fixture.pioneerRoot.string()) == 2);

        MetadataRestoreProposal proposal = proposalFor(fixture);
        proposal.cuesOffered = true;
        proposal.cues = storedCues();

        applyChange(fixture, proposal);

        assert(cueCount(fixture.pioneerRoot.string()) == 1);
        std::cout << "case 2: an offered cue set replaces what was there\n";
    }

    // Case 3: the two scales the format decides and no OneLibrary reader
    // reads back. A rating is stars, 0 to 5 -- four stars is a 4 in the
    // column and not Engine's 80 -- and the comment column is djComment,
    // which the schema spells that way and nothing else does.
    {
        Fixture fixture = freshFixture("annotation");
        MetadataRestoreProposal proposal = proposalFor(fixture);
        proposal.ratingOffered = true;
        proposal.rating = 4;
        proposal.commentOffered = true;
        proposal.comment = "mixes into the Larry Heard";

        applyChange(fixture, proposal);

        assert(storedRating(fixture.pioneerRoot.string()) == 4);
        assert(storedComment(fixture.pioneerRoot.string()) == "mixes into the Larry Heard");
        std::cout << "case 3: rating in stars, comment in djComment\n";
    }

    // Case 4: an absent field is left alone rather than cleared, so a
    // restore of one field cannot blank the other.
    {
        Fixture fixture = freshFixture("leaves_absent_alone");
        {
            MetadataRestoreProposal both = proposalFor(fixture);
            both.ratingOffered = true;
            both.rating = 5;
            both.commentOffered = true;
            both.comment = "keep me";
            applyChange(fixture, both);
        }

        MetadataRestoreProposal ratingOnly = proposalFor(fixture);
        ratingOnly.ratingOffered = true;
        ratingOnly.rating = 2;
        applyChange(fixture, ratingOnly);

        assert(storedRating(fixture.pioneerRoot.string()) == 2);
        assert(storedComment(fixture.pioneerRoot.string()) == "keep me");
        std::cout << "case 4: restoring one field leaves the other alone\n";
    }

    // Case 5: what the change calls itself. A proposal with no cues used
    // to describe itself as "Put 0 stored cue(s) back", which named a
    // write it does not make -- the summary the DJ reads before pressing
    // Save has to say what will actually happen.
    {
        Fixture fixture = freshFixture("description");
        MetadataRestoreProposal proposal = proposalFor(fixture);
        proposal.ratingOffered = true;
        proposal.rating = 3;
        proposal.commentOffered = true;
        proposal.comment = "a comment";
        const QString root = QString::fromStdString(fixture.pioneerRoot.string());
        RestoreMetadataChange change("onelibrary", root, "1", proposal);
        const QString description = change.description();
        assert(!description.contains("cue"));
        assert(description.contains("rating and comment"));
        assert(description.contains("Test Track"));

        MetadataRestoreProposal withCues = proposalFor(fixture);
        withCues.cuesOffered = true;
        withCues.cuesFillAGap = true;
        withCues.cues = storedCues();
        RestoreMetadataChange cueChange("onelibrary", root, "1", withCues);
        assert(cueChange.description().contains("1 stored cue(s)"));
        std::cout << "case 5: the description names the fields it will write\n";
    }

    // Case 6: what a restore costs in SQLCipher opens, and -- the part
    // that matters -- whether that cost is per save or per track.
    //
    // Every open derives the key from a passphrase, ~115 ms of CPU, so
    // on a real set this number is most of the feature's write cost.
    // Round 4 of docs/write-path-performance.md drove every save to a
    // flat floor of two, one write connection and one verify connection
    // held for the save, by putting the writer in SaveContext::shared.
    // This path did not reach it: RestoreMetadataChange keyed its shared
    // writer per sourceId, and OneLibraryCueWriterAdapter constructed a
    // fresh OneLibraryCueWriter on every call underneath -- two opens per
    // track, so a 400-track restore paid about 90 seconds of PBKDF2 and
    // nothing else in the suite would have noticed.
    //
    // Now flat. The assertion below is on the SHAPE, not just the
    // number: one track and three tracks must cost the same, which is
    // the only form of this that cannot quietly regress into linear
    // again.
    {
        // One save of N tracks, counted for N = 1 and N = 3. Two numbers
        // rather than one, because the question the counter has to
        // answer is not "how many" but "per save or per track": a cost
        // paid once per save is the floor working, and a cost that grows
        // with the track count is the Round 4 regression.
        auto opensForTrackCount = [](int trackCount) {
            Fixture fixture = freshFixture("open_count_" + std::to_string(trackCount));
            auto &noProgress = seabass::application::NullProgressReporter::instance();
            CancellationToken token;
            const QString root = QString::fromStdString(fixture.pioneerRoot.string());
            const std::string names[3] = {"Test Track", "Second", "Third"};

            seabass::infrastructure::WorkCounters::instance().reset();
            {
                SaveContext ctx(token, noProgress, {}, root, {});
                for (int i = 0; i < trackCount; ++i) {
                    MetadataRestoreProposal proposal;
                    proposal.storedId = "stored-" + std::to_string(i + 1);
                    proposal.stickTrack.sourceId = std::to_string(i + 1);
                    proposal.stickTrack.filePath =
                        (fixture.scratch / "Contents" / (names[i] + ".mp3")).string();
                    proposal.stickTrack.title = names[i];
                    proposal.cuesOffered = true;
                    proposal.cues = storedCues();
                    proposal.ratingOffered = true;
                    proposal.rating = 3;

                    RestoreMetadataChange change("onelibrary", root,
                                                 QString::fromStdString(proposal.stickTrack.sourceId), proposal);
                    assert(change.apply(ctx).ok);
                }
                assert(!ctx.runFinishHooks(true));
            }
            return seabass::infrastructure::WorkCounters::instance().snapshot().encryptedDatabaseOpens;
        };

        const auto one = opensForTrackCount(1);
        const auto three = opensForTrackCount(3);
        std::cout << "case 6: one track costs " << one << " SQLCipher open(s), three cost " << three
                  << " -- " << ((three - one) / 2) << " per extra track, against the floor of 2 per save"
                  << std::endl;

        // Two: the writer's own write connection and the separate
        // connection it verifies through, opened once and held. Flat in
        // the track count is the property; the constant is what the
        // format costs at all.
        assert(one == 2);
        assert(three == 2);
        assert(three == one);  // per save, not per track saved
        std::cout << "case 6: counted, and pinned so the fix has to move it\n";
    }

    // Case 7: two writers, one export.pdb, one save.
    //
    // Clean Up and Sync write export.pdb through a FormatWriteSession,
    // which -- for a batch big enough to earn it -- redirects them to a
    // local scratch copy and copies the whole file back onto the stick
    // when the save finishes. Restore Metadata writes a rating into the
    // same file. While each feature kept a session of its own, the two
    // wrote different files and whichever committed last won: the
    // session's copy, taken before the rating was written, silently
    // replaced it, and the page still reported the restore as applied.
    //
    // The session is now shared per database, so both write the one copy
    // that gets committed. This drives the real machinery -- a session
    // that really is scratching, and a real RestoreMetadataChange -- and
    // then reads the rating back off the stick's own export.pdb through
    // a fresh reader.
    {
        const fs::path scratch = seabass::testing::scratchRoot() / "seabass_restore_metadata_change_two_writers";
        std::error_code ec;
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch);
        const fs::path source =
            fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox";
        assert(fs::is_directory(source));
        const fs::path pioneer = scratch / "PIONEER";
        fs::copy(source, pioneer, fs::copy_options::recursive);

        seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(pioneer.string());
        const auto before = reader.readAll();
        assert(!before.empty());
        std::string targetId;
        std::string targetTitle;
        for (const auto &track : before) {
            if (!track.rating) {
                targetId = track.sourceId;
                targetTitle = track.title;
                break;
            }
        }
        assert(!targetId.empty());  // an unrated track, so the change is unambiguous

        auto &noProgress = seabass::application::NullProgressReporter::instance();
        CancellationToken token;
        const QString root = QString::fromStdString(pioneer.string());
        {
            SaveContext ctx(token, noProgress, {}, root, {});

            // Stand-in for a Clean Up or Sync change staged into the same
            // save: the session, asked for first and with a hint big
            // enough that it redirects to a scratch copy.
            auto &other = sharedFormatWriteSession(ctx, "rekordbox", pioneer.string(), 5000, "test-other-feature");
            assert(other.usesScratch());  // otherwise this case proves nothing

            MetadataRestoreProposal proposal;
            proposal.storedId = "stored-x";
            proposal.stickTrack.sourceId = targetId;
            proposal.stickTrack.title = targetTitle;
            proposal.ratingOffered = true;
            proposal.rating = 4;

            RestoreMetadataChange change("rekordbox", root, QString::fromStdString(targetId), proposal);
            assert(change.apply(ctx).ok);
            assert(!ctx.runFinishHooks(true));
        }

        // Off the stick's own file, through a reader that knows nothing
        // about any of the above.
        seabass::infrastructure::rekordbox::KaitaiRekordboxReader after(pioneer.string());
        bool found = false;
        for (const auto &track : after.readAll()) {
            if (track.sourceId == targetId) {
                found = true;
                assert(track.rating && *track.rating == 4);
            }
        }
        assert(found);
        std::cout << "case 7: a rating survives a save that also scratches export.pdb\n";
    }

    // Case 8: OneLibrary is never given a scratch copy, and a cancelled
    // save keeps what it already wrote.
    //
    // exportLibrary.db is a WAL database. A save holds its writers open
    // across the commit, so its committed rows are still in
    // exportLibrary.db-wal -- and FormatWriteSession commits by copying
    // the .db file alone. That loses this format's writes whichever root
    // they went to: through the scratch they are stranded in a -wal file
    // nobody copies, and at the real root they are overwritten by the
    // copy. So the session must decline the scratch for this format no
    // matter how large a batch asks for it.
    //
    // The second half is the reason the first half is not enough on its
    // own: FormatWriteSession discards a scratch copy on a cancel when
    // nothing was applied to it, so writes that DO go through one have
    // to be counted.
    {
        Fixture fixture = freshFixture("cancelled_save");
        auto &noProgress = seabass::application::NullProgressReporter::instance();
        CancellationToken token;
        const QString root = QString::fromStdString(fixture.pioneerRoot.string());
        {
            SaveContext ctx(token, noProgress, {}, root, {});
            // A hint far past the scratch threshold, from a stand-in for
            // some other feature staged into the same save. It must make
            // no difference.
            auto &session = sharedFormatWriteSession(ctx, "onelibrary", fixture.pioneerRoot.string(), 5000,
                                                     "test-other-feature");
            assert(!session.usesScratch());
            assert(session.writeRoot() == session.realRoot());

            MetadataRestoreProposal proposal = proposalFor(fixture);
            proposal.cuesOffered = true;
            proposal.cues = storedCues();

            RestoreMetadataChange change("onelibrary", root, "1", proposal);
            assert(change.apply(ctx).ok);
            // ok == false: the save was cancelled or failed after this
            // track went through.
            assert(!ctx.runFinishHooks(false));
        }
        assert(cueCount(fixture.pioneerRoot.string()) == 1);
        std::cout << "case 8: OneLibrary declines the scratch copy, and a cancel keeps the write\n";
    }

    // Case 9: a save that fails part-way keeps the ratings it already
    // committed.
    //
    // The rating goes into session.writeRoot()/rekordbox/export.pdb,
    // which IS the scratch copy when the save has one -- and
    // FormatWriteSession discards a scratch copy nothing was applied to.
    // While the rating went uncounted, a Stage All of four hundred
    // tracks that failed on track fifty-one threw away the fifty ratings
    // already committed, with those fifty changes already in the save's
    // applied list and their rows already off the page. Written,
    // reported, gone -- the failure the whole branch is about, reached
    // through the one path left uncounted.
    {
        const fs::path scratch = seabass::testing::scratchRoot() / "seabass_restore_metadata_change_partial";
        std::error_code ec;
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch);
        const fs::path source =
            fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox";
        const fs::path pioneer = scratch / "PIONEER";
        fs::copy(source, pioneer, fs::copy_options::recursive);

        seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(pioneer.string());
        std::string targetId;
        for (const auto &track : reader.readAll()) {
            if (!track.rating) {
                targetId = track.sourceId;
                break;
            }
        }
        assert(!targetId.empty());

        auto &noProgress = seabass::application::NullProgressReporter::instance();
        CancellationToken token;
        const QString root = QString::fromStdString(pioneer.string());
        {
            SaveContext ctx(token, noProgress, {}, root, {});
            // A batch big enough to earn the scratch copy, which is the
            // only situation in which the count matters at all.
            auto &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer.string(), 400, "test-batch");
            assert(session.usesScratch());

            MetadataRestoreProposal proposal;
            proposal.storedId = "stored-partial";
            proposal.stickTrack.sourceId = targetId;
            proposal.stickTrack.title = "Partial";
            proposal.ratingOffered = true;
            proposal.rating = 5;

            RestoreMetadataChange change("rekordbox", root, QString::fromStdString(targetId), proposal);
            assert(change.apply(ctx).ok);
            // ok == false: a later track in the same batch failed, or the
            // DJ cancelled. This one already committed its rating.
            assert(!ctx.runFinishHooks(false));
        }

        seabass::infrastructure::rekordbox::KaitaiRekordboxReader after(pioneer.string());
        bool found = false;
        for (const auto &track : after.readAll()) {
            if (track.sourceId == targetId) {
                found = true;
                assert(track.rating && *track.rating == 5);
            }
        }
        assert(found);
        std::cout << "case 9: a failed batch keeps the ratings it already committed\n";
    }

    std::cout << "restore_metadata_change_test: all cases passed\n";
    return 0;
}
