// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Keep this recording (application/use_cases/keep_recording.hpp) with fake
// ports: the order of the work, every point at which it can stop and what
// is left in the target folder then, the free space refusal, a target
// that exists, and the FLAC's file name. The fake file system keeps a
// log of every call, reads and writes apart, so the recording on the
// stick is proven never to be named in a write.

#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "application/use_cases/keep_recording.hpp"
#include "infrastructure/audio/recording_temp_names.hpp"

using namespace seabass::application;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (ok) {
        std::cout << "ok: " << what << "\n";
    } else {
        std::cout << "FAILED: " << what << "\n";
        ++failures;
    }
}

const std::string Wav = "/stick/Sessions/Session-0001.wav";
const std::string Folder = "/home/dj/Music/Own Recordings";
const std::string FileName = "DJ Kelp - Late Set.flac";
const std::string Flac = Folder + "/" + FileName;
const std::string Picture = "/home/dj/Pictures/cover.png";
const std::string Stick = "/stick";
// What the temporary names add to the FLAC's name, as the GUI hands it in:
// "." and 16 hex digits and ".seabass-encode-tmp", the longer suffix.
constexpr std::size_t TempExtra = seabass::infrastructure::audio::recordingTempNameExtraBytes();
static_assert(TempExtra == 1 + 16 + 19, "the temporary names' extra follows the suffix constants");
constexpr std::uint64_t WavBytes = 500ULL * 1024 * 1024;

// Everything the fakes did, in order: "read ...", "write ...", and the
// ports' own calls ("encode", "cover", "tags").
std::vector<std::string> journal;

class FakeFiles : public KeepRecordingFiles
{
public:
    std::set<std::string> present = {Wav, Picture};
    std::map<std::string, std::uint64_t> sizes = {{Wav, WavBytes}};
    std::optional<std::uint64_t> free = 10ULL * 1024 * 1024 * 1024;
    bool sizeUnknown = false;
    bool existsUnknown = false;
    bool pictureUnreadable = false;
    std::string folderRefusal;
    std::string removeRefusal;
    // isWithin()'s answer for the folder and the stick, and for the
    // recording and the stick.
    std::optional<bool> onStick = false;
    std::optional<bool> wavOnStick = true;
    // What sweepLeftovers() answers, and the FLAC names it asked about.
    std::vector<std::string> sweepNotes;
    std::vector<std::string> leftoverNames;
    std::vector<bool> leftoverInHand;
    std::optional<std::uint64_t> nameMax = 255;

    std::optional<std::uint64_t> fileSize(const std::string &path) override
    {
        journal.push_back("read size " + path);
        if (sizeUnknown || sizes.count(path) == 0) {
            return std::nullopt;
        }
        return sizes[path];
    }
    std::optional<bool> exists(const std::string &path) override
    {
        journal.push_back("read exists " + path);
        if (existsUnknown) {
            return std::nullopt;
        }
        return present.count(path) > 0;
    }
    std::optional<std::uint64_t> freeBytes(const std::string &folder) override
    {
        journal.push_back("read free " + folder);
        return free;
    }
    std::optional<std::string> readFile(const std::string &path, std::uint64_t) override
    {
        journal.push_back("read file " + path);
        if (pictureUnreadable || present.count(path) == 0) {
            return std::nullopt;
        }
        return std::string("PNG bytes");
    }
    std::optional<bool> isWithin(const std::string &path, const std::string &folder) override
    {
        journal.push_back("read within " + path + " in " + folder);
        return path == Wav ? wavOnStick : onStick;
    }
    std::optional<std::uint64_t> maxNameBytes(const std::string &folder) override
    {
        journal.push_back("read namemax " + folder);
        return nameMax;
    }
    std::string makeFolder(const std::string &folder) override
    {
        journal.push_back("write folder " + folder);
        if (!folderRefusal.empty()) {
            return folderRefusal;
        }
        present.insert(folder);
        return {};
    }
    std::string removeFile(const std::string &path) override
    {
        journal.push_back("write remove " + path);
        if (!removeRefusal.empty()) {
            return removeRefusal;
        }
        present.erase(path);
        sizes.erase(path);
        return {};
    }
    std::vector<std::string> sweepLeftovers(const std::string &folder,
                                            const std::function<bool(const std::string &)> &inHand) override
    {
        journal.push_back("write sweep " + folder);
        for (const std::string &name : leftoverNames) {
            leftoverInHand.push_back(inHand(name));
        }
        return sweepNotes;
    }
};

constexpr std::uint64_t EncodedBytes = 300ULL * 1024 * 1024;

class FakeEncoder : public RecordingEncoder
{
public:
    explicit FakeEncoder(FakeFiles &files) : m_files(files) {}

    std::string error;
    // Cancelled by the caller's token at this fraction (checked at each
    // report, as the real encoder polls).
    std::optional<double> cancelAt;
    CancellationToken *token = nullptr;
    // Called while the encoder runs: another job asked for meanwhile.
    std::function<void()> during;
    // With `error`: a file is left under the target's name, as the real
    // encoder leaves one whose size is wrong after the rename.
    bool leavesTarget = false;
    // Throws after putting a file under the target's name.
    bool throwsAfterTarget = false;
    // The recording's header is unfinished: refused without the choice,
    // and with it kept with `heldFrames` and `partialBytes` left out.
    bool unfinished = false;
    std::uint64_t heldFrames = 0;
    std::uint64_t partialBytes = 0;
    std::vector<std::string> notes;
    // The option the job was given.
    bool keepUnfinishedGiven = false;

    RecordingEncodeResult encode(const std::string &wavPath, const std::string &flacPath,
                                 const RecordingEncodeOptions &options) override
    {
        journal.push_back("encode " + wavPath + " -> " + flacPath);
        keepUnfinishedGiven = options.keepUnfinished;
        if (during) {
            during();
        }
        RecordingEncodeResult result;
        if (throwsAfterTarget) {
            m_files.present.insert(flacPath);
            m_files.sizes[flacPath] = 12;
            throw std::runtime_error("the encoder broke down");
        }
        if (unfinished) {
            result.unfinished = true;
            result.sampleRate = 48000;
            result.framesInWav = heldFrames;
            result.partialFrameBytesDropped = partialBytes;
            if (!options.keepUnfinished) {
                result.error = "Session-0001.wav cannot be encoded: the player did not finish this recording";
                return result;
            }
        }
        for (double f : {0.0, 0.3, 0.59, 0.6, 0.8, 1.0}) {
            if (cancelAt && f >= *cancelAt && token != nullptr) {
                token->cancel();
            }
            if (options.cancel.cancelled()) {
                result.cancelled = true;
                return result;
            }
            if (options.progress) {
                options.progress(f);
            }
        }
        if (!error.empty()) {
            if (leavesTarget) {
                m_files.present.insert(flacPath);
                m_files.sizes[flacPath] = 999;
            }
            result.error = error;
            return result;
        }
        m_files.present.insert(flacPath);
        m_files.sizes[flacPath] = EncodedBytes;
        result.encoded = true;
        result.notes = notes;
        return result;
    }

private:
    FakeFiles &m_files;
};

class FakeCovers : public CoverArtPreparer
{
public:
    std::string error;
    CoverArtResult prepare(const std::string &pictureBytes) override
    {
        journal.push_back("cover " + pictureBytes);
        if (!error.empty()) {
            return {std::nullopt, error};
        }
        return {CoverArt{"JPEG", 600, 600}, {}};
    }
};

class FakeTags : public RecordingTagWriter
{
public:
    explicit FakeTags(FakeFiles &files) : m_files(files) {}

    std::string error;
    // With `error`: another file stands under the FLAC's name afterwards,
    // as when the real writer finds the size wrong after its rename.
    bool replacesFile = false;
    bool throws = false;
    CancellationToken *cancelDuring = nullptr;
    RecordingTags written;
    RecordingTagWriteResult write(const std::string &flacPath, const RecordingTags &tags) override
    {
        journal.push_back("tags " + flacPath);
        written = tags;
        if (cancelDuring != nullptr) {
            cancelDuring->cancel();
        }
        if (throws) {
            throw std::runtime_error("the tag writer broke its word");
        }
        if (!error.empty()) {
            if (replacesFile) {
                m_files.sizes[flacPath] = EncodedBytes + 77;
            }
            return {false, error};
        }
        m_files.sizes[flacPath] = EncodedBytes + 4096;  // the tags and the cover
        return {true, {}};
    }

private:
    FakeFiles &m_files;
};

KeepRecordingRequest request()
{
    KeepRecordingRequest r;
    r.wavPath = Wav;
    r.folder = Folder;
    r.fileName = FileName;
    r.tags.artist = "DJ Kelp";
    r.tags.title = "Late Set";
    r.tags.album = "Own Recordings";
    r.tags.genre = "Techno";
    r.tags.year = 2026;
    r.coverPicturePath = Picture;
    r.stickRoot = Stick;
    r.tempNameExtraBytes = TempExtra;
    return r;
}

struct Run
{
    FakeFiles files;
    FakeEncoder encoder{files};
    FakeCovers covers;
    FakeTags tags{files};
    CancellationToken cancel;
    std::vector<KeepRecordingProgress> progress;
    KeepRecordingResult result;
    // Called at each progress report, to cancel at a step.
    std::function<void(const KeepRecordingProgress &)> onProgress;

    KeepRecordingResult go(const KeepRecordingRequest &r = request())
    {
        journal.clear();
        encoder.token = &cancel;
        result = keepRecording(r, files, encoder, covers, tags, cancel, [this](const KeepRecordingProgress &p) {
            progress.push_back(p);
            if (onProgress) {
                onProgress(p);
            }
        });
        return result;
    }

    bool called(const std::string &prefix) const
    {
        for (const std::string &line : journal) {
            if (line.rfind(prefix, 0) == 0) {
                return true;
            }
        }
        return false;
    }

    // Nothing of the job's in the folder: no FLAC.
    bool nothingLeft() const { return files.present.count(Flac) == 0; }

    // The WAV is named only in reads and in the encoder's input.
    bool wavOnlyRead() const
    {
        for (const std::string &line : journal) {
            if (line.find(Wav) == std::string::npos) {
                continue;
            }
            const bool read = line.rfind("read ", 0) == 0;
            const bool encoderInput = line.rfind("encode " + Wav + " -> ", 0) == 0;
            if (!read && !encoderInput) {
                return false;
            }
        }
        return files.present.count(Wav) == 1;
    }

    bool progressNeverBack() const
    {
        double last = 0.0;
        for (const auto &p : progress) {
            if (p.fraction < last) {
                return false;
            }
            last = p.fraction;
        }
        return true;
    }
};

void testTheWholeJobInOrder()
{
    Run run;
    run.go();
    check(run.result.status == KeepRecordingResult::Status::Kept, "kept");
    check(run.result.flacPath == Flac, "the FLAC is in the folder under its name");
    check(run.result.message.empty(), "nothing to say when kept");
    const std::vector<std::string> expected = {
        "read within " + Folder + " in " + Stick,
        "read within " + Wav + " in " + Stick,
        "read namemax " + Folder,
        "read size " + Wav,
        "write folder " + Folder,
        "write sweep " + Folder,
        "read exists " + Flac,
        "read free " + Folder,
        "encode " + Wav + " -> " + Flac,
        "read size " + Flac,
        "read file " + Picture,
        "cover PNG bytes",
        "tags " + Flac,
    };
    check(journal == expected, "space, then the encoding, then the cover, then the tags");
    if (journal != expected) {
        for (const auto &line : journal) {
            std::cout << "    " << line << "\n";
        }
    }
    check(run.tags.written.title == "Late Set" && run.tags.written.artist == "DJ Kelp"
              && run.tags.written.album == "Own Recordings" && run.tags.written.genre == "Techno"
              && run.tags.written.year == 2026,
          "every field reaches the tag writer");
    check(run.tags.written.cover.jpeg == "JPEG" && run.tags.written.cover.width == 600, "the prepared cover too");
    check(run.wavOnlyRead(), "the recording is only read");
    check(run.progressNeverBack(), "the bar never goes back");
    check(!run.progress.empty() && run.progress.back().fraction == 1.0, "the bar ends full");
    std::vector<KeepRecordingStep> steps;
    for (const auto &p : run.progress) {
        if (steps.empty() || steps.back() != p.step) {
            steps.push_back(p.step);
        }
    }
    check(steps
              == std::vector<KeepRecordingStep>{KeepRecordingStep::Space, KeepRecordingStep::Encoding,
                                                KeepRecordingStep::Checking, KeepRecordingStep::Tagging},
          "the steps come in order: space, encoding, checking, tags and cover");
}

void testWithoutCover()
{
    Run run;
    KeepRecordingRequest r = request();
    r.coverPicturePath.clear();
    run.go(r);
    check(run.result.status == KeepRecordingResult::Status::Kept, "kept without a cover");
    check(!run.called("read file") && !run.called("cover"), "no cover asked for, none read or prepared");
    check(run.tags.written.cover.jpeg.empty(), "and none written");
}

void testRefusedBeforeAnything()
{
    {
        Run run;
        KeepRecordingRequest r = request();
        r.tags.title.clear();
        run.go(r);
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("track name") != std::string::npos,
              "no track name: refused, saying so");
        check(journal.empty(), "no track name: nothing touched");
    }
    {
        Run run;
        run.files.sizeUnknown = true;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("could not be read") != std::string::npos,
              "a recording that cannot be read: refused");
        check(!run.called("write") && !run.called("encode"), "a recording that cannot be read: nothing written");
    }
    {
        Run run;
        run.files.folderRefusal = "a file is in the way";
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find(Folder) != std::string::npos
                  && run.result.message.find("a file is in the way") != std::string::npos,
              "a folder that cannot be made: refused, naming it and why");
        check(!run.called("encode"), "a folder that cannot be made: nothing encoded");
    }
    {
        Run run;
        run.files.present.insert(Flac);
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("already") != std::string::npos
                  && run.result.message.find(FileName) != std::string::npos,
              "a target that exists: refused, naming it");
        check(!run.called("encode") && !run.called("write remove"), "a target that exists: never encoded over, never removed");
        check(run.files.present.count(Flac) == 1, "a target that exists: still there");
    }
    {
        Run run;
        run.files.existsUnknown = true;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed && !run.called("encode"),
              "a folder that cannot be looked into: refused");
    }
}

void testFreeSpace()
{
    const std::uint64_t needed = keepRecordingSpaceNeeded(WavBytes);
    check(needed == 2 * WavBytes + KeepRecordingSpaceMargin, "room for twice the recording and a margin");
    {
        Run run;
        run.files.free = needed - 1;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("not enough room") != std::string::npos
                  && run.result.message.find("MB") != std::string::npos,
              "one byte short: refused, saying how much is needed and free");
        check(!run.called("encode"), "one byte short: nothing encoded");
        check(run.nothingLeft(), "one byte short: nothing left");
    }
    {
        Run run;
        run.files.free = needed;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Kept, "exactly enough: kept");
    }
    {
        Run run;
        run.files.free = std::nullopt;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Kept, "a volume that cannot tell its space: kept");
    }
}

void testEncoderFailsOrIsCancelled()
{
    {
        Run run;
        run.encoder.error = "Session-0001.wav was not encoded: the encoded file differs from the recording";
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message == run.encoder.error && run.result.leftBehind.empty(),
              "the encoder's failure: reported in its own words, nothing left");
        check(!run.called("tags") && !run.called("cover"), "the encoder's failure: nothing after it");
        check(run.nothingLeft() && run.wavOnlyRead(), "the encoder's failure: nothing left, the recording untouched");
    }
    {
        Run run;
        run.encoder.cancelAt = 0.3;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Cancelled && run.result.message.empty(),
              "cancelled while encoding: cancelled, nothing to explain");
        check(!run.called("tags") && run.nothingLeft() && run.wavOnlyRead(),
              "cancelled while encoding: nothing left, the recording untouched");
    }
    {
        Run run;
        run.encoder.cancelAt = 0.8;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Cancelled && run.nothingLeft(),
              "cancelled while checking: nothing left");
    }
    {
        Run run;
        run.onProgress = [&run](const KeepRecordingProgress &p) {
            if (p.step == KeepRecordingStep::Space) {
                run.cancel.cancel();
            }
        };
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Cancelled && !run.called("encode"),
              "cancelled before the encoding: never started");
        check(!run.called("write remove"), "cancelled before the encoding: nothing to take away");
    }
}

void testFailuresAfterTheEncodingRemoveTheFlac()
{
    {
        Run run;
        run.files.pictureUnreadable = true;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("cover") != std::string::npos,
              "a cover that cannot be read: failed, saying so");
        check(run.called("write remove " + Flac) && run.nothingLeft(), "a cover that cannot be read: the FLAC is removed");
        check(!run.called("tags"), "a cover that cannot be read: no tags written");
    }
    {
        Run run;
        run.covers.error = "not a picture";
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("not a picture") != std::string::npos,
              "a cover that cannot be prepared: failed, in the preparer's words");
        check(run.nothingLeft(), "a cover that cannot be prepared: the FLAC is removed");
    }
    {
        Run run;
        run.tags.error = "the copy's audio differs";
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("tags could not be written") != std::string::npos
                  && run.result.message.find("the copy's audio differs") != std::string::npos,
              "tags that cannot be written: failed, in the writer's words");
        check(run.nothingLeft() && run.wavOnlyRead(), "tags that cannot be written: the FLAC is removed, the recording untouched");
    }
    {
        // The tags were written and read back before the cancel was seen:
        // the job is done, and the FLAC is kept.
        Run run;
        run.tags.cancelDuring = &run.cancel;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Kept && run.result.flacPath == Flac,
              "cancelled while the tags are written, and they were: kept");
        check(!run.nothingLeft() && !run.called("write remove"), "cancelled after the tags: the FLAC stays");
    }
    {
        Run run;
        run.onProgress = [&run](const KeepRecordingProgress &p) {
            if (p.step == KeepRecordingStep::Tagging) {
                run.cancel.cancel();
            }
        };
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Cancelled && !run.called("tags"),
              "cancelled between the encoding and the tags: no tags written");
        check(run.nothingLeft(), "cancelled between the encoding and the tags: the FLAC is removed");
    }
    {
        Run run;
        run.tags.throws = true;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("broke its word") != std::string::npos,
              "a port that throws: failed, saying why");
        check(run.nothingLeft(), "a port that throws: the FLAC is removed");
    }
    {
        Run run;
        run.tags.error = "no room";
        run.files.removeRefusal = "it is held open";
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("no room") != std::string::npos
                  && run.result.message.find("could not be removed") != std::string::npos
                  && run.result.message.find("it is held open") != std::string::npos,
              "a FLAC that cannot be removed: said, beside why the job failed");
        check(run.result.leftBehind == Flac && run.result.message.find(Flac) != std::string::npos,
              "a FLAC that cannot be removed: left behind, with its path");
    }
    {
        // A cancel between the encoding and the tags, and the unfinished
        // FLAC cannot be removed: what was left, and where.
        Run run;
        run.files.removeRefusal = "it is held open";
        run.onProgress = [&run](const KeepRecordingProgress &p) {
            if (p.step == KeepRecordingStep::Tagging) {
                run.cancel.cancel();
            }
        };
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Cancelled && run.result.leftBehind == Flac
                  && run.result.message.find(Flac) != std::string::npos,
              "cancelled, and the FLAC cannot be removed: left behind, said with its path: " + run.result.message);
    }
}

// The encoder and the tag writer may answer "not kept" while a file stands
// under the FLAC's name (a check after the rename that failed): it is left
// as it is, said with its path, and never removed as if it were the job's.
void testAFileLeftInPlaceIsSaid()
{
    {
        Run run;
        run.encoder.error = "DJ Kelp - Late Set.flac does not have the size of the checked file after it was given its "
                            "name; it was left as it is and does not count as kept";
        run.encoder.leavesTarget = true;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed, "the encoder left a file: failed");
        check(run.result.leftBehind == Flac, "the encoder left a file: it is named as left behind");
        check(run.result.message.find(Flac) != std::string::npos
                  && run.result.message.find("does not count as kept") != std::string::npos,
              "the encoder left a file: the message gives its path: " + run.result.message);
        check(!run.called("write remove") && run.files.present.count(Flac) == 1, "the encoder left a file: never removed");
    }
    {
        Run run;
        run.tags.error = "the tags of DJ Kelp - Late Set.flac were written, but the file under its name does not have "
                         "the size of the checked copy afterwards";
        run.tags.replacesFile = true;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed && run.result.leftBehind == Flac,
              "the tag writer left another file: named as left behind");
        check(run.result.message.find(Flac) != std::string::npos
                  && run.result.message.find("was left") != std::string::npos,
              "the tag writer left another file: the message gives its path: " + run.result.message);
        check(!run.called("write remove") && run.files.present.count(Flac) == 1,
              "the tag writer left another file: never removed");
    }
    {
        Run run;
        run.encoder.error = "the encoder stopped";
        run.files.existsUnknown = false;
        run.go();
        check(run.result.leftBehind.empty() && run.result.message == "the encoder stopped",
              "nothing under the name after a failed encoding: nothing said about it");
    }
}

// The folder is on this computer, never on the stick; a name the drive
// cannot take and text a tag cannot hold are refused before anything.
void testThePlaceAndTheText()
{
    {
        Run run;
        run.files.onStick = true;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("is on the stick") != std::string::npos
                  && run.result.message.find("onto this computer") != std::string::npos,
              "a folder on the stick: refused, saying where the FLAC goes: " + run.result.message);
        check(!run.called("write") && !run.called("encode"), "a folder on the stick: nothing written");
    }
    {
        Run run;
        run.files.onStick = std::nullopt;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed && !run.called("write") && !run.called("encode"),
              "a folder that cannot be told from the stick: refused, nothing written");
    }
    {
        Run run;
        KeepRecordingRequest r = request();
        r.stickRoot.clear();
        run.go(r);
        check(run.result.status == KeepRecordingResult::Status::Failed && journal.empty(),
              "no stick named: refused before anything");
    }
    {
        Run run;
        run.files.nameMax = FileName.size() + TempExtra - 1;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("too long") != std::string::npos,
              "a name whose temporary name the drive cannot take: refused, saying so: " + run.result.message);
        check(!run.called("write") && !run.called("encode"), "a name too long: nothing written");
        Run fits;
        fits.files.nameMax = FileName.size() + TempExtra;
        fits.go();
        check(fits.result.status == KeepRecordingResult::Status::Kept, "a name that just fits with its temporary name: kept");
    }
    for (const std::string &bad : {std::string("DJ\tKelp"), std::string("DJ\nKelp"), std::string("DJ\x7f" "Kelp"),
                                   std::string("DJ\xe2\x80\xa8" "Kelp"), std::string("DJ\xe2\x80\xa9" "Kelp"),
                                   std::string("DJ\xc2\x85" "Kelp")}) {
        Run run;
        KeepRecordingRequest r = request();
        r.tags.genre = bad;
        run.go(r);
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("control character") != std::string::npos && journal.empty(),
              "a control character in a tag: refused before anything");
    }
    check(!hasControlCharacter("Nacht \xc3\xbc" "ber K\xc3\xb6ln \xe5\xa4\x9c \xe2\x80\x94 ok"), "letters, an em dash and CJK are fine");
}

// Two jobs never work on one recording or one FLAC: a second, asked for
// while the first encodes, is refused before it touches anything.
void testOneJobPerRecordingAndFlac()
{
    KeepRecordingResult sameWav;
    KeepRecordingResult sameFlac;
    std::vector<std::string> secondJournal;
    Run run;
    run.encoder.during = [&]() {
        const std::vector<std::string> outer = journal;
        Run other;
        KeepRecordingRequest r = request();
        r.fileName = "Another name.flac";
        sameWav = other.go(r);
        // The same FLAC, spelt with other letters' case and a backslash.
        Run third;
        KeepRecordingRequest t = request();
        t.wavPath = "/stick/Sessions/Session-0002.wav";
        t.fileName = "dj kelp - late set.FLAC";
        t.folder = "/home/dj/Music\\Own Recordings";
        sameFlac = third.go(t);
        secondJournal = journal;
        journal = outer;
    };
    run.go();
    check(run.result.status == KeepRecordingResult::Status::Kept, "the first job is kept");
    check(sameWav.status == KeepRecordingResult::Status::Failed
              && sameWav.message.find("another job is keeping it") != std::string::npos,
          "a second job for the same recording is refused: " + sameWav.message);
    check(sameFlac.status == KeepRecordingResult::Status::Failed
              && sameFlac.message.find("another job is writing") != std::string::npos,
          "a second job for the same FLAC is refused: " + sameFlac.message);
    check(secondJournal.empty(), "the refused jobs touched nothing");
    Run after;
    after.go();
    check(after.result.status == KeepRecordingResult::Status::Kept, "once the first has ended, the recording can be kept again");
}

// The recording is on the stick named; otherwise nothing is done.
void testTheRecordingIsOnTheStick()
{
    for (const std::optional<bool> answer : {std::optional<bool>(false), std::optional<bool>()}) {
        Run run;
        run.files.wavOnStick = answer;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed
                  && run.result.message.find("not on the stick") != std::string::npos,
              "a recording not on the stick named (or not known to be): refused: " + run.result.message);
        check(!run.called("write") && !run.called("encode"), "a recording not on the stick: nothing written");
    }
}

// An exception out of the encoder after it put a file under the target's
// name: the file is said, with its path, as after a failure.
void testAnEncoderExceptionSaysWhatIsLeft()
{
    Run run;
    run.encoder.throwsAfterTarget = true;
    run.go();
    check(run.result.status == KeepRecordingResult::Status::Failed
              && run.result.message.find("broke down") != std::string::npos,
          "an encoder that throws: failed, saying why: " + run.result.message);
    check(run.result.leftBehind == Flac && run.result.message.find(Flac) != std::string::npos,
          "an encoder that throws: the file under the target's name is said with its path: " + run.result.message);
    check(!run.called("write remove"), "an encoder that throws: that file is not removed as the job's");
}

// A recording the player did not finish: refused unless the person chose
// to keep what the file holds; when kept, the notes say so and how much.
void testAnUnfinishedRecording()
{
    {
        Run run;
        run.encoder.unfinished = true;
        run.encoder.heldFrames = 48000ULL * 3723;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed && !run.encoder.keepUnfinishedGiven,
              "unfinished, no choice: refused, and the encoder was not told to keep it");
        check(run.result.message.find("did not finish") != std::string::npos
                  && run.result.message.find("1:02:03") != std::string::npos
                  && run.result.message.find("Keep the audio the file holds") != std::string::npos,
              "unfinished, no choice: the message says what the file holds and how to keep it: " + run.result.message);
        check(run.result.unfinished && run.nothingLeft(), "unfinished, no choice: nothing left");
    }
    {
        Run run;
        run.encoder.unfinished = true;
        run.encoder.heldFrames = 48000ULL * 3723 + 17;
        run.encoder.partialBytes = 5;
        KeepRecordingRequest r = request();
        r.keepUnfinished = true;
        run.go(r);
        check(run.result.status == KeepRecordingResult::Status::Kept && run.encoder.keepUnfinishedGiven,
              "unfinished, chosen: kept, the choice handed to the encoder");
        check(run.result.unfinished && run.result.keptSeconds > 3723.0 && run.result.keptSeconds < 3723.01,
              "unfinished, chosen: the kept length is reported");
        const std::string notes = run.result.notes.empty() ? std::string() : run.result.notes.back();
        check(notes.find("did not finish") != std::string::npos && notes.find("1:02:03") != std::string::npos
                  && notes.find("5 bytes") != std::string::npos && notes.find("not changed") != std::string::npos,
              "unfinished, chosen: the notes say so, how much was kept and the partial frame left out: " + notes);
    }
    {
        Run run;
        KeepRecordingRequest r = request();
        r.keepUnfinished = true;
        run.go(r);
        check(run.result.status == KeepRecordingResult::Status::Kept && !run.result.unfinished
                  && run.result.notes.empty(),
              "a finished recording with the choice set: kept, nothing to note");
    }
}

// The sweep of leftovers runs at the start, in the target folder; its
// notes are the job's, and a FLAC in hand is reported as in hand.
void testTheSweep()
{
    Run run;
    run.files.sweepNotes = {"A temporary file an earlier keep left behind, more than a day old, was removed: x"};
    run.files.leftoverNames = {FileName, "Other.flac"};
    run.go();
    check(run.result.status == KeepRecordingResult::Status::Kept, "with leftovers: kept");
    check(run.result.notes == run.files.sweepNotes, "the sweep's notes are the job's");
    check(run.files.leftoverInHand == std::vector<bool>{true, false},
          "the job's own FLAC is in hand while it runs, another is not");
    Run encoderNotes;
    encoderNotes.encoder.notes = {"a second name"};
    encoderNotes.go();
    check(encoderNotes.result.notes == std::vector<std::string>{"a second name"}, "the encoder's notes are the job's");
}

// A job that ends in an exception lets go of its recording and its FLAC:
// the next job for both is not refused as one in hand.
void testTheRegistryLetsGoOnAnException()
{
    {
        Run run;
        run.tags.throws = true;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed, "a tag writer that throws: failed");
    }
    {
        Run run;
        run.encoder.throwsAfterTarget = true;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Failed, "an encoder that throws: failed");
    }
    Run after;
    after.go();
    check(after.result.status == KeepRecordingResult::Status::Kept,
          "after jobs that threw, the same recording and FLAC can be kept: " + after.result.message);
}

// A cut for the use case's own test: on a code point boundary. The GUI's
// cut by grapheme clusters is keep_recording_controller_test's.
std::size_t codePointCut(std::string_view text, std::size_t maxBytes)
{
    std::size_t at = std::min(maxBytes, text.size());
    while (at > 0 && at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
        --at;
    }
    return at;
}

void testFileNames()
{
    const auto name = [](const std::string &artist, const std::string &title) {
        return keptRecordingFileName(artist, title, keptRecordingStemBytes(MaxFileNameBytes, TempExtra), codePointCut);
    };
    const auto expect = [&](const std::string &artist, const std::string &title, const std::string &want,
                            const std::string &what) {
        const std::string got = name(artist, title);
        check(got == want, what + ": \"" + got + "\"");
    };
    expect("DJ Kelp", "Late Set", "DJ Kelp - Late Set.flac", "artist and track name");
    expect("", "Late Set", "Late Set.flac", "no artist: the track name alone");
    expect("Sebastián", "Nacht über Köln 夜", "Sebastián - Nacht über Köln 夜.flac", "non-ASCII is kept");
    expect("AC/DC", "Live: Who? \"Me\" <3> | *", "AC_DC - Live_ Who_ _Me_ _3_ _ _.flac",
           "characters FAT, exFAT and Windows refuse become '_'");
    expect("", "Back\\slash", "Back_slash.flac", "a backslash too");
    expect("", "Late Set... ", "Late Set.flac", "dots and spaces at the end go");
    expect("", " ..Late Set", "Late Set.flac", "and at the start");
    expect("", "Line\nbreak\tand\x7f", "Linebreakand.flac", "control characters go");
    expect("", "CON", "_CON.flac", "a name Windows reserves gets a '_'");
    expect("", "nul", "_nul.flac", "in any case");
    expect("", "COM1", "_COM1.flac", "COM1 too");
    expect("", "LPT9", "_LPT9.flac", "and LPT9");
    expect("CON", "Set", "CON - Set.flac", "a reserved word inside a longer name is fine");
    expect("", "Mix. Vol 2", "Mix. Vol 2.flac", "a dot inside stays");
    expect("...", "Set", "Set.flac", "an artist of nothing but dots is no artist");
    expect("DJ Kelp", "", "DJ Kelp.flac", "no track name: the artist alone");
    expect("", "", "Recording.flac", "nothing at all: Recording");
    const std::size_t stem = keptRecordingStemBytes(MaxFileNameBytes, TempExtra);
    check(stem == 255 - TempExtra - 5, "room for the stem: the name, less the temporary names' extra and .flac");
    check(keptRecordingStemBytes(40, TempExtra) == 0, "no room at all: none");
    const std::string longName = keptRecordingFileName("", std::string(300, 'x'), stem, codePointCut);
    check(longName.size() == stem + 5 && longName.substr(stem) == ".flac",
          "a long name is cut in bytes, leaving room for its temporary names: " + std::to_string(longName.size()));
    check(longName.size() + TempExtra <= MaxFileNameBytes, "and its longest temporary name fits 255 bytes");
    std::string umlauts;
    for (int i = 0; i < 200; ++i) {
        umlauts += "\xc3\xbc";  // ü, two bytes
    }
    const std::string cut = keptRecordingFileName("", umlauts, 101, codePointCut);
    check(cut.size() == 100 + 5, "cut where the cut says, never inside a UTF-8 sequence");
    const std::string spaced = keptRecordingFileName("", std::string(50, 'a') + "  . " + std::string(50, 'b'), 52,
                                                     codePointCut);
    check(spaced == std::string(50, 'a') + ".flac", "a space or a dot the cut leaves at the end goes too");

    check(suggestedRecordingTitle("Session-0001.wav") == "Session-0001", "suggested track name: the file name");
    check(suggestedRecordingTitle("my_set_2026.WAV") == "my set 2026", "underscores as spaces");
    check(suggestedRecordingTitle("REC001") == "REC001", "a name without an extension stays");
}

}  // namespace

int main()
{
    testTheWholeJobInOrder();
    testWithoutCover();
    testRefusedBeforeAnything();
    testFreeSpace();
    testEncoderFailsOrIsCancelled();
    testFailuresAfterTheEncodingRemoveTheFlac();
    testOneJobPerRecordingAndFlac();
    testAFileLeftInPlaceIsSaid();
    testThePlaceAndTheText();
    testFileNames();
    testTheRecordingIsOnTheStick();
    testAnEncoderExceptionSaysWhatIsLeft();
    testAnUnfinishedRecording();
    testTheSweep();
    testTheRegistryLetsGoOnAnException();
    if (failures > 0) {
        std::cout << "keep_recording_test: " << failures << " FAILED\n";
        return 1;
    }
    std::cout << "keep_recording_test: all passed\n";
    return 0;
}
