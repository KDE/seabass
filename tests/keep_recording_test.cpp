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
        return {};
    }
};

class FakeEncoder : public RecordingEncoder
{
public:
    explicit FakeEncoder(FakeFiles &files) : m_files(files) {}

    std::string error;
    // Cancelled by the caller's token at this fraction (checked at each
    // report, as the real encoder polls).
    std::optional<double> cancelAt;
    CancellationToken *token = nullptr;

    RecordingEncodeResult encode(const std::string &wavPath, const std::string &flacPath,
                                 const RecordingEncodeOptions &options) override
    {
        journal.push_back("encode " + wavPath + " -> " + flacPath);
        RecordingEncodeResult result;
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
            result.error = error;
            return result;
        }
        m_files.present.insert(flacPath);
        result.encoded = true;
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
    std::string error;
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
            return {false, error};
        }
        return {true, {}};
    }
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
    return r;
}

struct Run
{
    FakeFiles files;
    FakeEncoder encoder{files};
    FakeCovers covers;
    FakeTags tags;
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
        "read size " + Wav,
        "write folder " + Folder,
        "read exists " + Flac,
        "read free " + Folder,
        "encode " + Wav + " -> " + Flac,
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
                  && run.result.message == run.encoder.error,
              "the encoder's failure: reported in its own words");
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
        Run run;
        run.tags.cancelDuring = &run.cancel;
        run.go();
        check(run.result.status == KeepRecordingResult::Status::Cancelled,
              "cancelled while the tags are written: cancelled");
        check(run.nothingLeft(), "cancelled while the tags are written: the FLAC is removed");
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
    }
}

void testFileNames()
{
    const auto name = [](const std::string &artist, const std::string &title) {
        return keptRecordingFileName(artist, title);
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
    const std::string longName = name("", std::string(300, 'x'));
    check(longName.size() == 120 && longName.substr(115) == ".flac", "a long name is cut to 120 with its extension");
    std::string umlauts;
    for (int i = 0; i < 200; ++i) {
        umlauts += "\xc3\xbc";  // ü, one UTF-16 unit, two bytes
    }
    const std::string cut = name("", umlauts);
    check(cut.size() == 115 * 2 + 5, "cut on a character boundary, counted in UTF-16 units");

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
    testFileNames();
    if (failures > 0) {
        std::cout << "keep_recording_test: " << failures << " FAILED\n";
        return 1;
    }
    std::cout << "keep_recording_test: all passed\n";
    return 0;
}
