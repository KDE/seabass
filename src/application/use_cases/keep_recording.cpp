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
// Room for the name on every filesystem the computer may write to, as the
// import leaves for a name on the stick.
constexpr size_t MaxFileNameUtf16Units = 120;

std::string joinPath(const std::string &folder, const std::string &name)
{
    if (folder.empty()) {
        return name;
    }
    const char last = folder.back();
    return (last == '/' || last == '\\') ? folder + name : folder + "/" + name;
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
        if (!makeRoom()) {
            return m_result;
        }
        if (cancelled()) {
            return m_result;
        }
        if (!encode()) {
            return m_result;
        }
        // From here on the FLAC is this job's: anything that stops the job
        // takes it away again.
        m_flacMade = true;
        if (!tag()) {
            discardFlac();
            return m_result;
        }
        if (cancelled()) {
            discardFlac();
            return m_result;
        }
        m_flacMade = false;
        report(KeepRecordingStep::Tagging, 1.0);
        m_result.status = KeepRecordingResult::Status::Kept;
        m_result.flacPath = m_flacPath;
        m_result.message.clear();
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
        report(KeepRecordingStep::Encoding, 0.0);
        const RecordingEncodeResult encoded = m_encoder.encode(m_request.wavPath, m_flacPath, options);
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
                return fail("The cover " + m_request.coverPicturePath + " could not be read, so nothing was kept.");
            }
            const CoverArtResult cover = m_covers.prepare(*picture);
            if (!cover.cover) {
                return fail("The cover could not be used, so nothing was kept: " + cover.error);
            }
            tags.cover = *cover.cover;
        }
        if (cancelled()) {
            return false;
        }
        const RecordingTagWriteResult written = m_tags.write(m_flacPath, tags);
        if (!written.written) {
            return fail("The tags could not be written, so nothing was kept: " + written.error);
        }
        return true;
    }

public:
    // Takes the FLAC this job made away again; the person asked for a
    // tagged file, and a bare one is not that.
    void discardFlac()
    {
        if (!m_flacMade) {
            return;
        }
        m_flacMade = false;
        const std::string why = m_files.removeFile(m_flacPath);
        if (why.empty()) {
            return;
        }
        const std::string left = "The unfinished " + m_request.fileName + " could not be removed from "
            + m_request.folder + ": " + why;
        m_result.message = m_result.message.empty() ? left : m_result.message + " " + left;
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

std::string keptRecordingFileName(std::string_view artist, std::string_view title)
{
    std::string stem;
    const std::string safeArtist = domain::stickSafePathSegment(artist, "");
    const std::string safeTitle = domain::stickSafePathSegment(title, "");
    if (!safeArtist.empty() && !safeTitle.empty()) {
        stem = std::string(artist) + " - " + std::string(title);
    } else {
        stem = !safeTitle.empty() ? std::string(title) : std::string(artist);
    }
    return domain::stickSafePathSegment(stem, "Recording", MaxFileNameUtf16Units - FlacExtension.size())
        + std::string(FlacExtension);
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
        job.result().message = why;
        job.discardFlac();
        return job.result();
    } catch (...) {
        KeepRecordingResult result;
        result.message = why.empty() ? std::string("The recording was not kept.") : why;
        return result;
    }
}

}  // namespace seabass::application
