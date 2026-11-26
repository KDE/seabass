// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/keep_recording.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <mutex>
#include <set>

#include "domain/track_import_plan.hpp"

namespace seabass::application
{

namespace
{

// How much of the job's bar the encoder's run is (its encoding and its
// check); the cover and the tags are the rest. The tag writer copies the
// FLAC once and compares its audio: a fraction of the encoding's time.
constexpr double EncoderShareOfJob = 0.9;

constexpr std::string_view FlacExtension = ".flac";
// No cut by stickSafePathSegment(): the name is cut in bytes, on a
// character boundary, by keptRecordingFileName().
constexpr size_t Uncut = static_cast<size_t>(-1);

std::string joinPath(const std::string &folder, const std::string &name)
{
    if (folder.empty()) {
        return name;
    }
    const char last = folder.back();
    return (last == '/' || last == '\\') ? folder + name : folder + "/" + name;
}

// "1:02:03" or "2:03".
std::string clockDuration(std::uint64_t frames, int sampleRate)
{
    const std::uint64_t seconds = sampleRate > 0 ? frames / static_cast<std::uint64_t>(sampleRate) : 0;
    const auto two = [](std::uint64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    const std::uint64_t h = seconds / 3600;
    const std::uint64_t m = seconds / 60 % 60;
    return h > 0 ? std::to_string(h) + ":" + two(m) + ":" + two(seconds % 60)
                 : std::to_string(m) + ":" + two(seconds % 60);
}

std::string megabytes(std::uint64_t bytes)
{
    const auto mb = static_cast<std::uint64_t>(std::ceil(static_cast<double>(bytes) / (1024.0 * 1024.0)));
    return std::to_string(mb) + " MB";
}

// The recordings and the FLACs that jobs in this process have in hand.
// Two jobs never work on one: the second is refused before it touches
// anything. Compared as written, with '\\' as '/' and ASCII letters folded,
// so that a filesystem that ignores case is covered; the form compares the
// paths as files before a job is queued.
class InHand
{
public:
    InHand(const std::string &wavPath, const std::string &flacPath)
        : m_wav("wav " + key(wavPath)), m_flac("flac " + key(flacPath))
    {
        const std::lock_guard<std::mutex> lock(mutex());
        if (held().count(m_wav) > 0) {
            m_refusal = Refusal::Wav;
        } else if (held().count(m_flac) > 0) {
            m_refusal = Refusal::Flac;
        } else {
            held().insert(m_wav);
            held().insert(m_flac);
        }
    }
    ~InHand()
    {
        if (m_refusal == Refusal::None) {
            const std::lock_guard<std::mutex> lock(mutex());
            held().erase(m_wav);
            held().erase(m_flac);
        }
    }
    InHand(const InHand &) = delete;
    InHand &operator=(const InHand &) = delete;

    enum class Refusal
    {
        None,
        Wav,
        Flac,
    };
    Refusal refusal() const { return m_refusal; }

    // Whether a job of this process has the FLAC at `flacPath` in hand.
    static bool flacHeld(const std::string &flacPath)
    {
        const std::lock_guard<std::mutex> lock(mutex());
        return held().count("flac " + key(flacPath)) > 0;
    }

private:
    static std::string key(std::string path)
    {
        for (char &c : path) {
            if (c == '\\') {
                c = '/';
            } else if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return path;
    }
    static std::mutex &mutex()
    {
        static std::mutex m;
        return m;
    }
    static std::set<std::string> &held()
    {
        static std::set<std::string> keys;
        return keys;
    }

    std::string m_wav;
    std::string m_flac;
    Refusal m_refusal = Refusal::None;
};

class Job
{
public:
    Job(const KeepRecordingRequest &request, KeepRecordingFiles &files, RecordingEncoder &encoder,
        CoverArtPreparer &covers, RecordingTagWriter &tags, const CancellationToken &cancel,
        const std::function<void(const KeepRecordingProgress &)> &progress)
        : m_request(request),
          m_files(files),
          m_encoder(encoder),
          m_covers(covers),
          m_tags(tags),
          m_cancel(cancel),
          m_progress(progress),
          m_flacPath(joinPath(request.folder, request.fileName))
    {
    }

    KeepRecordingResult run()
    {
        report(KeepRecordingStep::Space, 0.0);
        if (!checkRequest()) {
            return m_result;
        }
        const InHand inHand(m_request.wavPath, m_flacPath);
        if (inHand.refusal() == InHand::Refusal::Wav) {
            fail("The recording was not kept: another job is keeping it right now.");
            return m_result;
        }
        if (inHand.refusal() == InHand::Refusal::Flac) {
            fail("The recording was not kept: another job is writing " + m_request.fileName + " in "
                 + m_request.folder + " right now.");
            return m_result;
        }
        if (!checkPlace()) {
            return m_result;
        }
        if (!makeRoom()) {
            return m_result;
        }
        if (cancelled()) {
            return m_result;
        }
        m_encodeStarted = true;
        if (!encode()) {
            sayWhatIsLeft();
            return m_result;
        }
        // From here on the FLAC is this job's: anything that stops the job
        // before the tags are written takes it away again. Its size is
        // what tells it from a file that took its place.
        m_flacMade = true;
        m_encodedBytes = m_files.fileSize(m_flacPath);
        if (!tag()) {
            discardFlac();
            return m_result;
        }
        // The tags are written and read back: the job is done, and a
        // cancel that comes now is too late to take it back.
        m_result.status = KeepRecordingResult::Status::Kept;
        m_result.flacPath = m_flacPath;
        m_result.message.clear();
        m_result.leftBehind.clear();
        if (m_result.unfinished) {
            const std::string what = m_declaredDuration
                ? "The header of this recording was not completed: it declares " + *m_declaredDuration
                    + " of audio, the file held " + m_keptDuration + "."
                : "The header of this recording was not completed and did not say how long the audio is, so the "
                  "file was read to its end.";
            m_result.notes.push_back(what + " The FLAC holds " + m_keptDuration
                                     + " of audio, every whole frame the file held"
                                     + (m_partialBytes > 0 ? "; the last " + std::to_string(m_partialBytes)
                                                                 + " bytes, less than a frame, were left out."
                                                           : ".")
                                     + " The recording on the stick was not changed.");
        }
        // A progress callback that throws now does not make it a failure.
        try {
            report(KeepRecordingStep::Tagging, 1.0);
        } catch (...) {
        }
        return m_result;
    }

private:
    void report(KeepRecordingStep step, double fraction)
    {
        m_fraction = std::clamp(fraction, m_fraction, 1.0);
        if (m_progress) {
            m_progress(KeepRecordingProgress{step, m_fraction});
        }
    }

    bool fail(const std::string &why)
    {
        m_result.status = KeepRecordingResult::Status::Failed;
        m_result.message = why;
        return false;
    }

    bool cancelled()
    {
        if (!m_cancel.cancelled()) {
            return false;
        }
        m_result.status = KeepRecordingResult::Status::Cancelled;
        m_result.message.clear();
        return true;
    }

    bool checkRequest()
    {
        if (m_request.tags.title.empty()) {
            return fail("The recording was not kept: it has no track name.");
        }
        if (m_request.folder.empty()) {
            return fail("The recording was not kept: no folder was chosen for it.");
        }
        if (m_request.fileName.empty() || m_request.fileName.find_first_of("/\\") != std::string::npos) {
            return fail("The recording was not kept: \"" + m_request.fileName + "\" is not a file name.");
        }
        if (m_request.stickRoot.empty()) {
            return fail("The recording was not kept: it is not known which stick it is on.");
        }
        const std::pair<const char *, const std::string *> fields[] = {
            {"artist", &m_request.tags.artist},
            {"track name", &m_request.tags.title},
            {"album", &m_request.tags.album},
            {"genre", &m_request.tags.genre},
        };
        for (const auto &[name, text] : fields) {
            if (hasControlCharacter(*text)) {
                return fail(std::string("The recording was not kept: the ") + name
                            + " holds a line break, a tab or another control character.");
            }
        }
        return true;
    }

    // The folder is not on the stick, and the FLAC's name and its longest
    // temporary name fit its filesystem.
    bool checkPlace()
    {
        const std::optional<bool> onStick = m_files.isWithin(m_request.folder, m_request.stickRoot);
        if (!onStick) {
            return fail("The recording was not kept: it could not be told whether " + m_request.folder
                        + " is on the stick, and the FLAC file goes onto this computer only.");
        }
        if (*onStick) {
            return fail("The recording was not kept: " + m_request.folder
                        + " is on the stick. The FLAC file goes onto this computer; the stick keeps the recording.");
        }
        // The stick named is the recording's: otherwise the check above
        // would compare the folder with some other place.
        const std::optional<bool> wavOnStick = m_files.isWithin(m_request.wavPath, m_request.stickRoot);
        if (wavOnStick != std::optional<bool>(true)) {
            return fail("The recording was not kept: it is not on the stick " + m_request.stickRoot
                        + ", or that could not be told.");
        }
        const std::uint64_t longest =
            std::min<std::uint64_t>(MaxFileNameBytes, m_files.maxNameBytes(m_request.folder).value_or(MaxFileNameBytes));
        const std::uint64_t needed = m_request.fileName.size() + m_request.tempNameExtraBytes;
        if (needed > longest) {
            return fail("The recording was not kept: the name " + m_request.fileName + " is too long for the drive of "
                        + m_request.folder + ". Make the artist or the track name shorter.");
        }
        return true;
    }

    // The folder, made when it is missing, and the room in it.
    bool makeRoom()
    {
        const std::optional<std::uint64_t> wavBytes = m_files.fileSize(m_request.wavPath);
        if (!wavBytes) {
            return fail("The recording was not kept: it could not be read on the stick.");
        }
        const std::string made = m_files.makeFolder(m_request.folder);
        if (!made.empty()) {
            return fail("The folder " + m_request.folder + " could not be made: " + made);
        }
        // Leftovers of earlier jobs that were killed: in this folder only,
        // older than a day, and none a job of this process has in hand.
        const std::string folder = m_request.folder;
        for (std::string &note : m_files.sweepLeftovers(folder, [&folder](const std::string &flacName) {
                 return InHand::flacHeld(joinPath(folder, flacName));
             })) {
            m_result.notes.push_back(std::move(note));
        }
        const std::optional<bool> taken = m_files.exists(m_flacPath);
        if (!taken) {
            return fail("The folder " + m_request.folder + " could not be looked into.");
        }
        if (*taken) {
            return fail(m_request.fileName + " is in " + m_request.folder
                        + " already, and is not replaced. Choose another name.");
        }
        // A volume that cannot tell its free space is not refused for it:
        // the encoder and the tag writer prove what they write.
        if (const std::optional<std::uint64_t> free = m_files.freeBytes(m_request.folder)) {
            const std::uint64_t needed = keepRecordingSpaceNeeded(*wavBytes);
            if (*free < needed) {
                return fail("There is not enough room in " + m_request.folder + ": keeping this recording needs up to "
                            + megabytes(needed) + ", and " + megabytes(*free) + " are free.");
            }
        }
        return true;
    }

    bool encode()
    {
        RecordingEncodeOptions options;
        options.cancel = m_cancel;
        options.progress = [this](double fraction) {
            const KeepRecordingStep step =
                fraction < RecordingEncodingShare ? KeepRecordingStep::Encoding : KeepRecordingStep::Checking;
            report(step, EncoderShareOfJob * fraction);
        };
        options.keepUnfinished = m_request.keepUnfinished;
        report(KeepRecordingStep::Encoding, 0.0);
        const RecordingEncodeResult encoded = m_encoder.encode(m_request.wavPath, m_flacPath, options);
        m_result.unfinished = encoded.unfinished;
        for (const std::string &note : encoded.notes) {
            m_result.notes.push_back(note);
        }
        if (encoded.unfinished) {
            m_keptDuration = clockDuration(encoded.framesInWav, encoded.sampleRate);
            m_partialBytes = encoded.partialFrameBytesDropped;
            if (encoded.framesDeclared) {
                m_declaredDuration = clockDuration(*encoded.framesDeclared, encoded.sampleRate);
            }
            if (encoded.sampleRate > 0) {
                m_result.keptSeconds =
                    static_cast<double>(encoded.framesInWav) / static_cast<double>(encoded.sampleRate);
            }
        }
        if (encoded.unfinished && !m_request.keepUnfinished && !encoded.encoded && !encoded.cancelled) {
            const std::string what = m_declaredDuration
                ? "its header was not completed. The header declares " + *m_declaredDuration
                    + " of audio, the file holds " + m_keptDuration + "."
                : "its header was not completed and does not say how long the audio is. The file holds "
                    + m_keptDuration + " of audio.";
            return fail("The recording was not kept: " + what
                        + " This happens when a player is switched off or a stick is pulled while recording. "
                          "Choose \"Keep the audio the file holds\" to keep that.");
        }
        if (encoded.cancelled) {
            m_result.status = KeepRecordingResult::Status::Cancelled;
            m_result.message.clear();
            return false;
        }
        if (!encoded.encoded) {
            return fail(encoded.error.empty() ? std::string("The recording was not encoded.") : encoded.error);
        }
        return true;
    }

    bool tag()
    {
        report(KeepRecordingStep::Tagging, EncoderShareOfJob);
        if (cancelled()) {
            return false;
        }
        RecordingTags tags = m_request.tags;
        if (!m_request.coverPicturePath.empty()) {
            const std::optional<std::string> picture =
                m_files.readFile(m_request.coverPicturePath, MaxCoverPictureBytes);
            if (!picture) {
                return fail("The cover " + m_request.coverPicturePath + " could not be read, so the recording was not kept.");
            }
            const CoverArtResult cover = m_covers.prepare(*picture);
            if (!cover.cover) {
                return fail("The cover could not be used, so the recording was not kept: " + cover.error);
            }
            tags.cover = *cover.cover;
        }
        if (cancelled()) {
            return false;
        }
        const RecordingTagWriteResult written = m_tags.write(m_flacPath, tags);
        if (!written.written) {
            return fail("The tags could not be written, so the recording was not kept: " + written.error);
        }
        return true;
    }

    void addToMessage(const std::string &sentence)
    {
        m_result.message = m_result.message.empty() ? sentence : m_result.message + " " + sentence;
    }

    // After an encoding that did not end in a FLAC: the encoder takes its
    // own temporary file away, but a failed check after the rename leaves
    // the file under the target's name where it is (it may be another
    // program's). Nothing was there before the encoding, so whatever is
    // there now is said, never left unsaid.
    void sayWhatIsLeft()
    {
        const std::optional<bool> there = m_files.exists(m_flacPath);
        if (there == std::optional<bool>(false)) {
            return;
        }
        if (!there) {
            addToMessage("Whether a file named " + m_request.fileName + " was left in " + m_request.folder
                         + " could not be told.");
            return;
        }
        m_result.leftBehind = m_flacPath;
        addToMessage("A file named " + m_request.fileName + " is in " + m_request.folder
                     + " now. It does not count as kept, and it was left as it is: " + m_flacPath);
    }

public:
    // After an exception: whatever the job had put in the folder is taken
    // away or said, as after a failure.
    void afterException()
    {
        if (m_flacMade) {
            discardFlac();
        } else if (m_encodeStarted) {
            sayWhatIsLeft();
        }
    }

    // Takes the FLAC this job made away again; the person asked for a
    // tagged file, and a bare one is not that. Only while it is the file
    // the encoder made (its size unchanged): a file the tag writer put in
    // its place, or one that took its place otherwise, is left where it
    // is and said.
    //
    // That size guard covers every way the tag writer can fail: before its
    // rename the FLAC is the encoder's, untouched; after it, the tagged
    // copy has the size the writer checked, and a written=false then
    // means another size is there. A file that vanished and was replaced
    // by something else of exactly the encoder's size is not guarded
    // against: far-fetched, and nothing short of a hash would tell it.
    void discardFlac()
    {
        if (!m_flacMade) {
            return;
        }
        m_flacMade = false;
        const std::optional<bool> there = m_files.exists(m_flacPath);
        if (there == std::optional<bool>(false)) {
            return;
        }
        const std::optional<std::uint64_t> size = there ? m_files.fileSize(m_flacPath) : std::nullopt;
        if (!there || !size || !m_encodedBytes || *size != *m_encodedBytes) {
            m_result.leftBehind = m_flacPath;
            addToMessage("A file named " + m_request.fileName + " was left in " + m_request.folder
                         + ": it is not the file that was checked, so it does not count as kept, and it was left as it is: "
                         + m_flacPath);
            return;
        }
        const std::string why = m_files.removeFile(m_flacPath);
        if (why.empty()) {
            return;
        }
        m_result.leftBehind = m_flacPath;
        addToMessage("The unfinished " + m_request.fileName + " could not be removed from " + m_request.folder + " ("
                     + why + "). It does not count as kept: " + m_flacPath);
    }

    KeepRecordingResult &result() { return m_result; }

private:

    const KeepRecordingRequest &m_request;
    KeepRecordingFiles &m_files;
    RecordingEncoder &m_encoder;
    CoverArtPreparer &m_covers;
    RecordingTagWriter &m_tags;
    CancellationToken m_cancel;
    const std::function<void(const KeepRecordingProgress &)> &m_progress;
    std::string m_flacPath;
    KeepRecordingResult m_result;
    double m_fraction = 0.0;
    bool m_flacMade = false;
    bool m_encodeStarted = false;
    std::optional<std::uint64_t> m_encodedBytes;
    std::string m_keptDuration;
    // What the header declares; nullopt when it declares no size.
    std::optional<std::string> m_declaredDuration;
    std::uint64_t m_partialBytes = 0;
};

}  // namespace

const char *keepRecordingStepKey(KeepRecordingStep step)
{
    switch (step) {
        case KeepRecordingStep::Space:
            return "space";
        case KeepRecordingStep::Encoding:
            return "encoding";
        case KeepRecordingStep::Checking:
            return "checking";
        case KeepRecordingStep::Tagging:
            return "tags";
    }
    return "space";
}

std::uint64_t keepRecordingSpaceNeeded(std::uint64_t wavBytes)
{
    return 2 * wavBytes + KeepRecordingSpaceMargin;
}

bool hasControlCharacter(std::string_view text)
{
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7F) {
            return true;
        }
        // U+0085 (NEXT LINE) is C2 85; U+2028 and U+2029 are E2 80 A8 and
        // E2 80 A9.
        if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x85) {
            return true;
        }
        if (c == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80) {
            const auto third = static_cast<unsigned char>(text[i + 2]);
            if (third == 0xA8 || third == 0xA9) {
                return true;
            }
        }
    }
    return false;
}

std::size_t keptRecordingStemBytes(std::size_t maxNameBytes, std::size_t tempNameExtraBytes)
{
    const std::size_t taken = tempNameExtraBytes + FlacExtension.size();
    return maxNameBytes > taken ? maxNameBytes - taken : 0;
}

std::string keptRecordingFileName(std::string_view artist, std::string_view title, std::size_t maxStemBytes,
                                  const GraphemeCut &cut)
{
    std::string stem;
    const std::string safeArtist = domain::stickSafePathSegment(artist, "", Uncut);
    const std::string safeTitle = domain::stickSafePathSegment(title, "", Uncut);
    if (!safeArtist.empty() && !safeTitle.empty()) {
        stem = std::string(artist) + " - " + std::string(title);
    } else {
        stem = !safeTitle.empty() ? std::string(title) : std::string(artist);
    }
    std::string safe = domain::stickSafePathSegment(stem, "Recording", Uncut);
    if (safe.size() > maxStemBytes) {
        const std::size_t keep = cut ? std::min(cut(safe, maxStemBytes), maxStemBytes) : 0;
        // Cut, then made safe again: a space or a dot the cut left at the
        // end goes too.
        safe = domain::stickSafePathSegment(std::string_view(safe).substr(0, keep), "Recording", Uncut);
        if (safe.size() > maxStemBytes) {
            safe = "Recording";
        }
    }
    return safe + std::string(FlacExtension);
}

std::string suggestedRecordingTitle(std::string_view recordingFileName)
{
    std::string name(recordingFileName);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name.erase(dot);
    }
    std::replace(name.begin(), name.end(), '_', ' ');
    const size_t first = name.find_first_not_of(' ');
    const size_t last = name.find_last_not_of(' ');
    return first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
}

KeepRecordingResult keepRecording(const KeepRecordingRequest &request, KeepRecordingFiles &files,
                                  RecordingEncoder &encoder, CoverArtPreparer &covers, RecordingTagWriter &tags,
                                  const CancellationToken &cancel,
                                  const std::function<void(const KeepRecordingProgress &)> &progress)
{
    std::string why;
    try {
        Job job(request, files, encoder, covers, tags, cancel, progress);
        try {
            return job.run();
        } catch (const std::exception &e) {
            why = std::string("The recording was not kept: ") + e.what();
        } catch (...) {
            why = "The recording was not kept.";
        }
        job.result().status = KeepRecordingResult::Status::Failed;
        const std::string before = job.result().message;
        job.result().message = before.empty() ? why : why + " " + before;
        job.afterException();
        return job.result();
    } catch (...) {
        KeepRecordingResult result;
        result.message = why.empty() ? std::string("The recording was not kept.") : why;
        return result;
    }
}

}  // namespace seabass::application
