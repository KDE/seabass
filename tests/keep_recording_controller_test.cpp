// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// KeepRecordingController (gui/keep_recording_controller.hpp) with the real
// encoder, cover preparer and tag writer on short WAVs this test writes
// (recording_test_support.hpp: tones and noise, no real audio) into a
// scratch folder: the form's suggestions and checks, a tagged FLAC with
// its cover, progress that never goes back and ends full, a second job
// that waits for the first, a cancel in the middle (and the jobs it
// drops), a failure that stays in the strip while the next job runs, a
// cancel while the tags are written (nothing kept) and one right after the
// FLAC was placed (too late to undo the job), a file left in place said
// with its path, a folder on the stick refused, a name cut to what the
// drive and the temporary name leave room for, on a character boundary, text a tag
// cannot hold refused, and the controller destroyed in the middle of a
// job, in the encoder and in the tag writer. What is in the target folder
// afterwards is read from the disk here. The settings store is a scratch
// file on every platform, proven before anything is written.

#include <QDir>
#include <QDeadlineTimer>
#include <QWaitCondition>
#include <QMutex>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/xiphcomment.h>

#include "gui/keep_recording_controller.hpp"
#include "gui/qt_path.hpp"
#include "gui/seabass_settings.hpp"
#include "infrastructure/audio/qt_cover_art_preparer.hpp"
#include "infrastructure/audio/qt_flac_recording_encoder.hpp"
#include "infrastructure/audio/taglib_recording_tag_writer.hpp"
#include "infrastructure/local/local_keep_recording_files.hpp"
#include "recording_test_support.hpp"
#include "scratch_path.hpp"

using seabass::gui::KeepRecordingController;
using seabass::gui::KeepRecordingParts;
namespace fs = std::filesystem;

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

std::string field(TagLib::Ogg::XiphComment *comment, const char *name)
{
    if (comment == nullptr || !comment->contains(name)) {
        return "<absent>";
    }
    const TagLib::StringList values = comment->fieldListMap()[name];
    return values.size() == 1 ? values.front().to8Bit(true) : "<" + std::to_string(values.size()) + " values>";
}

// Every entry in a folder, by name; empty for a folder that is not there.
std::vector<std::string> entries(const QString &folder)
{
    std::vector<std::string> names;
    for (const QString &name : QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        names.push_back(name.toStdString());
    }
    return names;
}

bool waitUntil(const std::function<bool()> &done, int ms = 60000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return done();
}

// The stick the recordings are on, in scratch: its root, with Sessions/.
QString makeStick(const QString &scratch)
{
    const QString root = scratch + QStringLiteral("/STICK");
    QDir().mkpath(root + QStringLiteral("/Sessions"));
    return root;
}

// A WAV on the scratch folder's stick (makeStick()): a job keeps only a
// recording that is on the stick it is told of. A folder already in the
// stick is taken as it is.
std::string writeWav(const QString &scratchOrFolder, const QString &name, double seconds)
{
    const QString dir = scratchOrFolder.endsWith(QStringLiteral("/Sessions"))
        ? scratchOrFolder : scratchOrFolder + QStringLiteral("/STICK/Sessions");
    QDir().mkpath(dir);
    const std::string path = QDir(dir).filePath(name).toStdString();
    recording_test::WavSpec spec;
    spec.frames = static_cast<std::uint64_t>(44100 * seconds);
    recording_test::writeWav(path, spec);
    return path;
}

QVariantMap form(const std::string &wav, const QString &folder, const QString &artist, const QString &title)
{
    return {
        {"wavPath", QString::fromStdString(wav)},
        {"recording", QFileInfo(QString::fromStdString(wav)).fileName()},
        {"artist", artist},
        {"title", title},
        {"album", QStringLiteral("Own Recordings")},
        {"genre", QStringLiteral("Techno")},
        {"year", QStringLiteral("2026")},
        {"folder", folder},
    };
}

// Records the progress of each job as the controller shows it.
struct ProgressLog
{
    explicit ProgressLog(KeepRecordingController &controller)
    {
        QObject::connect(&controller, &KeepRecordingController::progressChanged, &controller, [this, &controller]() {
            if (!controller.busy()) {
                return;
            }
            const QString wav = controller.currentWavPath();
            if (byJob.empty() || byJob.back().first != wav) {
                byJob.push_back({wav, {}});
            }
            byJob.back().second.push_back(controller.progress());
        });
    }
    std::vector<std::pair<QString, std::vector<double>>> byJob;
};

bool neverBack(const std::vector<double> &values)
{
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i] < values[i - 1]) {
            return false;
        }
    }
    return true;
}

void suggestionsAndChecks(const QString &scratch)
{
    KeepRecordingController controller;
    controller.setStickRoot(makeStick(scratch));
    const QVariantMap first = controller.suggestionsFor(QStringLiteral("my_set_0001.wav"),
                                                        QDateTime(QDate(2025, 7, 12), QTime(2, 30)));
    check(first.value("title").toString() == QStringLiteral("my set 0001"), "suggested track name from the file name");
    check(first.value("year").toInt() == 2025, "suggested year from the recording's date");
    check(first.value("album").toString() == QStringLiteral("Own Recordings"), "suggested album");
    check(first.value("artist").toString().isEmpty() && first.value("genre").toString().isEmpty(),
          "no artist or genre before the first time");
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    check(first.value("folder").toString() == QDir::toNativeSeparators(QDir(music).filePath("Own Recordings")),
          "the folder starts as Own Recordings in the Music folder: " + first.value("folder").toString().toStdString());
    check(controller.suggestionsFor(QStringLiteral("x.wav"), QDateTime()).value("year").toInt() == 0,
          "no date, no year");

    check(controller.fileNameFor(QStringLiteral(" AC/DC "), QStringLiteral("Live?"), scratch)
              == QStringLiteral("AC_DC - Live_.flac"),
          "the file name is made safe");
    check(controller.yearProblem(QString()).isEmpty() && controller.yearProblem(QStringLiteral("1900")).isEmpty()
              && controller.yearProblem(QStringLiteral("2100")).isEmpty(),
          "a year: empty, 1900 and 2100 are fine");
    check(!controller.yearProblem(QStringLiteral("1899")).isEmpty() && !controller.yearProblem(QStringLiteral("2101")).isEmpty()
              && !controller.yearProblem(QStringLiteral("20x6")).isEmpty(),
          "a year: 1899, 2101 and not a number are refused");

    const QString blocked = scratch + QStringLiteral("/blocked");
    QDir().mkpath(scratch);
    {
        QFile file(blocked);
        file.open(QIODevice::WriteOnly);
        file.write("in the way");
    }
    check(controller.folderProblem(blocked).contains(QStringLiteral("file")), "a file where the folder should be: said");
    check(controller.folderProblem(blocked + QStringLiteral("/below")).contains(QStringLiteral("in the way")),
          "a file in the way of a folder to be made: said");
    check(controller.folderProblem(scratch + QStringLiteral("/to be made/deeper")).isEmpty(),
          "a folder that is not there yet is made on start");
    check(!controller.folderProblem(QStringLiteral("relative/path")).isEmpty(), "a relative folder is refused");
    check(!controller.folderProblem(QString()).isEmpty(), "no folder is refused");

    const QString target = scratch + QStringLiteral("/taken");
    QDir().mkpath(target);
    {
        QFile file(target + QStringLiteral("/DJ Kelp - Set.flac"));
        file.open(QIODevice::WriteOnly);
        file.write("someone's file");
    }
    check(controller.targetProblem(target, QStringLiteral("DJ Kelp"), QStringLiteral("Set")).contains(QStringLiteral("already")),
          "a file of that name in the folder: said before starting");
    const std::string wav = writeWav(scratch, QStringLiteral("refused.wav"), 0.5);
    check(!controller.keep(form(wav, target, QStringLiteral("DJ Kelp"), QStringLiteral("Set"))) && !controller.busy(),
          "keep() refuses a name that is taken");
    check(!controller.errorMessage().isEmpty(), "and says why");
    check(!controller.keep(form(wav, target, QStringLiteral("DJ Kelp"), QString())), "keep() refuses no track name");
    QVariantMap badYear = form(wav, target, QStringLiteral("DJ Kelp"), QStringLiteral("Other"));
    badYear.insert("year", QStringLiteral("1850"));
    check(!controller.keep(badYear), "keep() refuses a year out of range");
    check(!controller.keep(form(wav, blocked, QStringLiteral("DJ Kelp"), QStringLiteral("Other"))),
          "keep() refuses a folder with a file in its place");
    check(entries(target) == std::vector<std::string>{"DJ Kelp - Set.flac"}, "nothing was written by a refusal");
}

void twoJobsQueued(const QString &scratch)
{
    const QString stick = makeStick(scratch);
    const QString folder = scratch + QStringLiteral("/kept/Own Recordings é");
    const std::string wav1 = writeWav(scratch, QStringLiteral("Session-0001.wav"), 3);
    const std::string wav2 = writeWav(scratch, QStringLiteral("Session-0002.wav"), 2);
    const QString picture = scratch + QStringLiteral("/cover.png");
    {
        QImage image(2000, 1000, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QPainter painter(&image);
        painter.fillRect(100, 100, 600, 400, Qt::yellow);
        painter.end();
        image.save(picture);
    }

    KeepRecordingController controller;
    controller.setStickRoot(stick);
    check(controller.available(), "this build can keep a recording");
    ProgressLog log(controller);
    std::vector<std::string> events;
    QObject::connect(&controller, &KeepRecordingController::busyChanged, &controller, [&]() {
        if (controller.busy()) {
            events.push_back("start " + controller.currentRecording().toStdString());
        }
    });
    QObject::connect(&controller, &KeepRecordingController::finished, &controller, [&](const QVariantMap &outcome) {
        events.push_back("end " + outcome.value("recording").toString().toStdString() + " "
                         + outcome.value("state").toString().toStdString());
    });

    controller.pickCover(picture);
    check(waitUntil([&]() { return !controller.coverPreparing(); }), "the cover preview is prepared");
    check(controller.coverError().isEmpty(), "the cover can be used: " + controller.coverError().toStdString());
    check(controller.coverPreview().startsWith(QStringLiteral("data:image/jpeg;base64,")), "the preview is the prepared JPEG");
    check(controller.coverWidth() == 1200 && controller.coverHeight() == 600, "the preview has the size it is embedded at");

    // White space around the text is not kept.
    check(controller.keep(form(wav1, folder, QStringLiteral("  DJ Kelp "), QStringLiteral(" Late Set  "))),
          "first job taken");
    check(controller.busy(), "and running");
    controller.pickCover(QString());
    check(controller.keep(form(wav2, folder, QStringLiteral("DJ Kelp"), QStringLiteral("Early Set"))), "second job taken");
    check(controller.queued().size() == 1, "the second waits in the queue");
    check(controller.outcomeFor(stick, QString::fromStdString(wav2)).value("state").toString() == QStringLiteral("queued"),
          "its row says it waits");
    check(controller.targetProblem(folder, QStringLiteral("DJ Kelp"), QStringLiteral("Early Set")).contains(QStringLiteral("Another recording")),
          "a third with the name of one that waits is refused before it starts");
    check(controller.targetProblem(folder + QStringLiteral("/../") + QFileInfo(folder).fileName(), QStringLiteral("dj kelp"),
                                   QStringLiteral("EARLY SET"))
              .contains(QStringLiteral("Another recording")),
          "the same name spelt with other case and another path to the folder is refused too");
    check(!controller.keep(form(wav2, folder, QStringLiteral("DJ Kelp"), QStringLiteral("Third Name")))
              && controller.errorMessage().contains(QStringLiteral("waiting")),
          "a second job for a recording that waits is refused: " + controller.errorMessage().toStdString());
    check(!controller.keep(form(wav1, folder, QStringLiteral("DJ Kelp"), QStringLiteral("Fourth Name")))
              && controller.errorMessage().contains(QStringLiteral("right now")),
          "a second job for the recording in hand is refused: " + controller.errorMessage().toStdString());
    check(controller.queued().size() == 1, "and nothing more waits");

    check(waitUntil([&]() { return events.size() >= 4; }), "both jobs end");
    check(events
              == std::vector<std::string>{"start Session-0001.wav", "end Session-0001.wav kept", "start Session-0002.wav",
                                          "end Session-0002.wav kept"},
          "the second starts only after the first has ended");
    for (const auto &line : events) {
        std::cout << "    " << line << "\n";
    }
    check(!controller.busy() && controller.queued().isEmpty(), "nothing runs or waits afterwards");

    check(log.byJob.size() == 2, "progress was shown for both jobs");
    for (const auto &[wav, values] : log.byJob) {
        check(neverBack(values), "progress never goes back: " + wav.toStdString());
        check(!values.empty() && values.back() == 1.0, "progress ends at 1: " + wav.toStdString());
        check(values.size() >= 3, "progress moves on the way (" + std::to_string(values.size()) + " steps)");
    }

    const QVariantMap kept = controller.outcomeFor(stick, QString::fromStdString(wav1));
    check(kept.value("state").toString() == QStringLiteral("kept"), "the row says kept");
    const QString flac1 = folder + QStringLiteral("/DJ Kelp - Late Set.flac");
    check(QDir::cleanPath(QDir::fromNativeSeparators(kept.value("flacPath").toString())) == QDir::cleanPath(flac1),
          "and where: " + kept.value("flacPath").toString().toStdString());
    check(entries(folder) == (std::vector<std::string>{"DJ Kelp - Early Set.flac", "DJ Kelp - Late Set.flac"}),
          "the folder holds the two FLACs and nothing else");
    const QVariantList ended = controller.finishedJobs();
    check(ended.size() == 2 && ended.last().toMap().value("recording").toString() == QStringLiteral("Session-0002.wav"),
          "the strip lists both jobs, the one that ended last at the end");

    {
        const fs::path path = seabass::pathFromUtf8(flac1.toStdString());
        TagLib::FLAC::File file(path.c_str(), false);
        TagLib::Ogg::XiphComment *comment = file.xiphComment(false);
        check(field(comment, "ARTIST") == "DJ Kelp" && field(comment, "TITLE") == "Late Set"
                  && field(comment, "ALBUM") == "Own Recordings" && field(comment, "GENRE") == "Techno"
                  && field(comment, "DATE") == "2026",
              "the FLAC's tags read back");
        const auto pictures = file.pictureList();
        check(pictures.size() == 1 && pictures.front()->type() == TagLib::FLAC::Picture::FrontCover
                  && pictures.front()->width() == 1200 && pictures.front()->height() == 600,
              "the FLAC has the front cover, scaled");
    }
    {
        const fs::path path = seabass::pathFromUtf8((folder + QStringLiteral("/DJ Kelp - Early Set.flac")).toStdString());
        TagLib::FLAC::File file(path.c_str(), false);
        check(file.pictureList().isEmpty(), "the second, asked for without a cover, has none");
    }
    recording_test::WavSpec spec;
    spec.frames = 44100 * 3;
    check(recording_test::compareWithSignal(recording_test::decodeAll(flac1.toStdString()), spec).empty(),
          "the FLAC decodes to the recording, sample for sample");

    QSettings settings;
    const QVariantMap next = controller.suggestionsFor(QStringLiteral("Session-0003.wav"), QDateTime());
    check(next.value("artist").toString() == QStringLiteral("DJ Kelp") && next.value("genre").toString() == QStringLiteral("Techno")
              && next.value("folder").toString() == QDir::toNativeSeparators(folder),
          "artist, genre and folder are suggested from the last time");
}

void cancelInTheMiddle(const QString &scratch)
{
    const QString folder = scratch + QStringLiteral("/cancelled");
    const std::string wav = writeWav(scratch, QStringLiteral("long.wav"), 240);
    const std::string waiting = writeWav(scratch, QStringLiteral("waiting.wav"), 1);
    const std::string waiting2 = writeWav(scratch, QStringLiteral("waiting too.wav"), 1);
    KeepRecordingController controller;
    const QString stick = makeStick(scratch);
    controller.setStickRoot(stick);
    bool asked = false;
    QObject::connect(&controller, &KeepRecordingController::progressChanged, &controller, [&]() {
        if (!asked && controller.progress() >= 0.2) {
            asked = true;
            controller.cancelFor(stick);
        }
    });
    QSignalSpy finished(&controller, &KeepRecordingController::finished);
    check(controller.keep(form(wav, folder, QString(), QStringLiteral("Long"))), "a long job taken");
    check(controller.keep(form(waiting, folder, QString(), QStringLiteral("Waiting"))), "and one behind it");
    check(controller.keep(form(waiting2, folder, QString(), QStringLiteral("Waiting Too"))), "and another");
    check(waitUntil([&]() { return finished.count() >= 1 && !controller.busy(); }), "the cancelled job ends");
    check(asked, "cancelled in the middle, at a fifth");
    check(finished.count() == 1, "only the job in hand ends: the one that waited was dropped");
    const QVariantMap outcome = finished.isEmpty() ? QVariantMap() : finished.first().first().toMap();
    check(outcome.value("state").toString() == QStringLiteral("cancelled"), "it says cancelled");
    check(controller.queued().isEmpty(), "nothing waits after a cancel");
    check(entries(folder).empty(), "no file of it in the folder, no temporary file either");
    const QVariantMap stopped = controller.outcomeFor(stick, QString::fromStdString(wav));
    check(stopped.value("state").toString() == QStringLiteral("cancelled") && stopped.value("leftBehind").toString().isEmpty(),
          "the row of the job in hand says it was stopped, nothing left");
    for (const std::string &path : {waiting, waiting2}) {
        check(controller.outcomeFor(stick, QString::fromStdString(path)).value("state").toString() == QStringLiteral("dropped"),
              "a job that waited has an outcome of its own: dropped");
    }
    const QVariantList ended = controller.finishedJobs();
    QStringList states;
    for (const QVariant &job : ended) {
        states << job.toMap().value("state").toString() + QLatin1Char(' ') + job.toMap().value("recording").toString();
    }
    check(states == (QStringList{QStringLiteral("dropped waiting.wav"), QStringLiteral("dropped waiting too.wav"),
                                 QStringLiteral("cancelled long.wav")}),
          "the strip lists the dropped jobs and the stopped one: " + states.join(QStringLiteral(", ")).toStdString());
    const QVariantMap notes = controller.keepNotesFor(
        stick, {QString::fromStdString(wav), QString::fromStdString(waiting), QString::fromStdString(waiting2),
         QStringLiteral("/never/asked.wav")});
    check(notes.value("notKept").toStringList()
              == (QStringList{QStringLiteral("long.wav"), QStringLiteral("waiting.wav"), QStringLiteral("waiting too.wav")}),
          "the delete confirmation can name every one not kept, and none never asked for");
    check(notes.value("kept").toList().isEmpty(), "and none as kept");
    controller.dismissFinishedFor(stick);
    check(controller.finishedJobs().isEmpty(), "closed, the strip's list is empty");
    check(controller.outcomeFor(stick, QString::fromStdString(waiting)).value("state").toString() == QStringLiteral("dropped"),
          "the rows still say it");
    check(fs::file_size(seabass::pathFromUtf8(wav)) == 44 + 44100ULL * 240 * 4, "the recording is as it was");
}

// How the destructor's waits for its workers ended: what, and whether on
// the worker's own signal before the deadline.
struct StopWaits
{
    std::vector<std::pair<std::string, bool>> seen;
    StopWaits()
    {
        KeepRecordingController::setStopWaitObserverForTesting(
            [this](const char *what, bool ended) { seen.emplace_back(what, ended); });
    }
    ~StopWaits() { KeepRecordingController::setStopWaitObserverForTesting({}); }
    StopWaits(const StopWaits &) = delete;
    StopWaits &operator=(const StopWaits &) = delete;
};

// A hang guard only, for a latch nobody lets go of: far above anything
// the destructor may take.
constexpr qint64 DestructorHangGuardMs = 30000;

// What the destructor may take at the most: KeepRecordingStopWaitMs for
// each of its two workers, and generous slack for a loaded machine. A
// destructor that waited for a held worker would take the hang guard.
constexpr qint64 DestructorBoundMs = 2 * seabass::gui::KeepRecordingStopWaitMs + 5000;

// Held until the test lets it go (or the hang guard passes): a step the
// destructor cannot interrupt, without a wait of fixed length.
class Latch
{
public:
    void release()
    {
        const QMutexLocker lock(&m_mutex);
        m_released = true;
        m_wake.wakeAll();
    }
    // True when released, false when the hang guard passed first.
    bool wait()
    {
        QMutexLocker lock(&m_mutex);
        const QDeadlineTimer guard(DestructorHangGuardMs);
        while (!m_released) {
            if (!m_wake.wait(&m_mutex, guard)) {
                break;
            }
        }
        m_left = true;
        return m_released;
    }
    void markEntered() { m_entered = true; }
    bool entered() const { return m_entered.load(); }
    // Whether the step held by the latch has returned, by a release or the
    // hang guard.
    bool left() const { return m_left.load(); }

private:
    QMutex m_mutex;
    QWaitCondition m_wake;
    bool m_released = false;
    std::atomic<bool> m_entered{false};
    std::atomic<bool> m_left{false};
};

void destroyedMidEncoding(const QString &scratch)
{
    const QString folder = scratch + QStringLiteral("/destroyed");
    const std::string wav = writeWav(scratch, QStringLiteral("destroyed.wav"), 240);
    auto controller = std::make_unique<KeepRecordingController>();
    controller->setStickRoot(makeStick(scratch));
    check(controller->keep(form(wav, folder, QString(), QStringLiteral("Gone"))), "a job taken, to be left");
    check(waitUntil([&]() { return controller->progress() >= 0.1; }), "it is under way");
    StopWaits waits;
    QElapsedTimer timer;
    timer.start();
    controller.reset();
    const qint64 ms = timer.elapsed();
    check(ms < DestructorBoundMs, "the controller goes within its wait and some slack: " + std::to_string(ms) + " ms");
    // Woken as the worker ends, not at the end of the wait.
    check(waits.seen == (std::vector<std::pair<std::string, bool>>{{"job", true}}),
          "the destructor's wait for the job ended on the worker's own signal, the cancel answered in time");
    QThreadPool::globalInstance()->waitForDone();
    check(entries(folder).empty(), "no file left in the folder");
}

// A tag writer held until the test lets it go: the step the destructor
// cannot interrupt.
class HeldTags : public seabass::application::RecordingTagWriter
{
public:
    explicit HeldTags(Latch *latch) : m_latch(latch) {}
    seabass::application::RecordingTagWriteResult tagInPlace(const seabass::application::VerifiedFlac &file,
                                                             const seabass::application::RecordingTags &) override
    {
        m_latch->markEntered();
        m_latch->wait();
        return {true, {}, file};
    }

private:
    Latch *m_latch;
};

void destroyedMidTagging(const QString &scratch)
{
    const QString folder = scratch + QStringLiteral("/destroyed-tagging");
    const std::string wav = writeWav(scratch, QStringLiteral("tagging.wav"), 2);
    Latch latch;
    KeepRecordingController::setPartsForTesting([&latch]() {
        KeepRecordingParts parts;
        parts.files = std::make_unique<seabass::infrastructure::LocalKeepRecordingFiles>();
        parts.encoder = std::make_unique<seabass::infrastructure::audio::QtFlacRecordingEncoder>();
        parts.covers = std::make_unique<seabass::infrastructure::audio::QtCoverArtPreparer>();
        parts.tags = std::make_unique<HeldTags>(&latch);
        return parts;
    });
    auto controller = std::make_unique<KeepRecordingController>();
    controller->setStickRoot(makeStick(scratch));
    check(controller->keep(form(wav, folder, QString(), QStringLiteral("Slow"))), "a job taken, to be left while tagged");
    check(waitUntil([&]() { return latch.entered(); }), "it reaches the tags, and the tag writer holds");
    StopWaits waits;
    QElapsedTimer timer;
    timer.start();
    controller.reset();
    const qint64 ms = timer.elapsed();
    // Back while the tag writer still holds: the destructor did not wait
    // for it, it gave up at its deadline (and the worker is left to end).
    const bool stillHeld = !latch.left();
    check(stillHeld, "the tag writer still holds when the controller is gone: the destructor did not wait for it");
    check(ms < DestructorBoundMs, "the controller goes while the tags are written, within its wait and some slack: "
                                      + std::to_string(ms) + " ms");
    check(waits.seen == (std::vector<std::pair<std::string, bool>>{{"job", false}}),
          "the destructor's wait for the job gave up at its deadline: the tag writer had not returned");
    latch.release();
    QThreadPool::globalInstance()->waitForDone();
    check(latch.left(), "let go, the tag writer returns and the worker ends");
    // The tag writer said written, but the FLAC was not placed yet: the
    // cancel that came while it wrote is seen before the placing, and
    // nothing is kept, no temporary file either.
    check(entries(folder).empty(), "cancelled while the tags were written: nothing kept, nothing left in the folder");
    KeepRecordingController::setPartsForTesting({});
}

// A cover preparer held until the test lets it go, as a huge picture on a
// slow disk.
class HeldCovers : public seabass::application::CoverArtPreparer
{
public:
    explicit HeldCovers(Latch *latch) : m_latch(latch) {}
    seabass::application::CoverArtResult prepare(const std::string &) override
    {
        m_latch->markEntered();
        m_latch->wait();
        return {std::nullopt, "too slow"};
    }

private:
    Latch *m_latch;
};

// Leaving while a cover preview is under way waits for it until the
// deadline, not until it ends.
void destroyedMidCoverPreview(const QString &scratch)
{
    QDir().mkpath(scratch);
    const QString picture = scratch + QStringLiteral("/slow.png");
    QImage(64, 64, QImage::Format_RGB32).save(picture);
    Latch latch;
    KeepRecordingController::setPartsForTesting([&latch]() {
        KeepRecordingParts parts;
        parts.files = std::make_unique<seabass::infrastructure::LocalKeepRecordingFiles>();
        parts.encoder = std::make_unique<seabass::infrastructure::audio::QtFlacRecordingEncoder>();
        parts.covers = std::make_unique<HeldCovers>(&latch);
        parts.tags = std::make_unique<HeldTags>(&latch);
        return parts;
    });
    auto controller = std::make_unique<KeepRecordingController>();
    controller->pickCover(picture);
    check(controller->coverPreparing(), "a cover preview is under way");
    check(waitUntil([&]() { return latch.entered(); }), "and the preparer holds");
    StopWaits waits;
    QElapsedTimer timer;
    timer.start();
    controller.reset();
    const qint64 ms = timer.elapsed();
    const bool stillHeld = !latch.left();
    check(stillHeld, "the preparer still holds when the controller is gone: the destructor did not wait for it");
    check(ms < DestructorBoundMs, "the controller goes while a cover is prepared, within its wait and some slack: "
                                      + std::to_string(ms) + " ms");
    check(waits.seen == (std::vector<std::pair<std::string, bool>>{{"cover", false}}),
          "the destructor's wait for the preview gave up at its deadline: the preparer had not returned");
    latch.release();
    QThreadPool::globalInstance()->waitForDone();
    check(latch.left(), "let go, the preparer returns and the worker ends");
    KeepRecordingController::setPartsForTesting({});
}


// A folder on the stick is refused, in the form and in keep(): the page
// lists every audio file in Sessions/, so a FLAC there would be deleted
// with its WAV.
void folderOnTheStick(const QString &scratch)
{
    const QString stick = makeStick(scratch);
    const std::string wav = writeWav(stick + QStringLiteral("/Sessions"), QStringLiteral("Session-0001.wav"), 0.5);
    KeepRecordingController controller;
    controller.setStickRoot(stick);
    const auto refused = [&](const QString &folder) {
        return controller.folderProblem(folder).contains(QStringLiteral("on the stick"));
    };
    check(refused(stick), "the stick's root is refused");
    check(refused(stick + QStringLiteral("/Sessions")), "the recording's own folder is refused");
    check(refused(stick + QStringLiteral("/Kept/Deeper")), "a folder still to be made on the stick is refused");
    check(refused(stick + QStringLiteral("/Sessions/../Kept")), "another spelling of a folder on the stick is refused");
    check(refused(QDir::toNativeSeparators(stick + QStringLiteral("/Sessions"))), "the native spelling too");
    check(refused(QUrl::fromLocalFile(stick + QStringLiteral("/Sessions")).toString()), "and a file: URL");
    const QString link = scratch + QStringLiteral("/link to the stick");
    check(QFile::link(stick, link), "a link to the stick is made");
    check(refused(link + QStringLiteral("/Own Recordings")), "a folder reached through a link to the stick is refused");
    check(controller.folderProblem(stick + QStringLiteral("/Sessions")).contains(QStringLiteral("onto this computer")),
          "and the form says where the FLAC goes: " + controller.folderProblem(stick).toStdString());
    const QString beside = scratch + QStringLiteral("/STICK2/Own Recordings");
    check(controller.folderProblem(beside).isEmpty(),
          "a folder beside the stick, whose name starts like it, is fine: " + controller.folderProblem(beside).toStdString());

    const std::vector<std::string> before = entries(stick + QStringLiteral("/Sessions"));
    check(!controller.keep(form(wav, stick + QStringLiteral("/Sessions"), QStringLiteral("DJ Kelp"), QStringLiteral("Set"))),
          "keep() refuses a folder on the stick");
    check(controller.errorMessage().contains(QStringLiteral("on the stick")) && !controller.busy(), "and says why");
    check(entries(stick + QStringLiteral("/Sessions")) == before, "nothing was written on the stick");

    KeepRecordingController unknown;
    check(!unknown.folderProblem(scratch + QStringLiteral("/Own Recordings")).isEmpty(),
          "without a stick named, no folder is taken");
}

// The FLAC's name, cut to what the folder's drive leaves room for beside
// the temporary names, on a character boundary; the form shows it as it
// is written.
void longNames(const QString &scratch)
{
    const QString stick = makeStick(scratch);
    const QString folder = scratch + QStringLiteral("/long");
    KeepRecordingController controller;
    controller.setStickRoot(stick);

    // Pieces that are one character each but several code points: an e
    // with a combining accent, a family of four (one emoji sequence of
    // seven code points), a flag (two regional indicators).
    const QString accent = QStringLiteral("e") + QChar(0x0301);
    const QString family = QString::fromUtf8("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7"
                                             "\xe2\x80\x8d\xf0\x9f\x91\xa6");
    const QString flag = QString::fromUtf8("\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa");
    for (const QString &piece : {accent, family, flag, QStringLiteral("x")}) {
        for (const QString &lead : {QString(), QStringLiteral("a"), QStringLiteral("ab"), QStringLiteral("abc")}) {
            QString title = lead;
            while (title.toUtf8().size() < 600) {
                title += piece;
            }
            const QString name = controller.fileNameFor(QString(), title, folder);
            const QString stem = name.chopped(5);
            check(name.endsWith(QStringLiteral(".flac")), "the name ends in .flac");
            check(name.toUtf8().size() <= 255, "the name is at most 255 bytes");
            QString whole = lead;
            while (whole.size() < stem.size()) {
                whole += piece;
            }
            check(whole == stem, "cut between two characters, never inside one: "
                                     + std::to_string(stem.toUtf8().size()) + " bytes");
        }
    }

    // Kept for real: the encoder's and the tag writer's temporary names,
    // longer than the FLAC's, fit the drive too.
    const std::string wav = writeWav(scratch, QStringLiteral("long.wav"), 0.5);
    QString title;
    while (title.toUtf8().size() < 600) {
        title += QString::fromUtf8("\xc3\xbc") + family;
    }
    const QString expected = controller.fileNameFor(QStringLiteral("DJ Kelp"), title, folder);
    QSignalSpy finished(&controller, &KeepRecordingController::finished);
    check(controller.keep(form(wav, folder, QStringLiteral("DJ Kelp"), title)), "a recording with a very long name is taken");
    check(waitUntil([&]() { return finished.count() >= 1; }), "and ends");
    const QVariantMap outcome = finished.isEmpty() ? QVariantMap() : finished.first().first().toMap();
    check(outcome.value("state").toString() == QStringLiteral("kept"),
          "it is kept: " + outcome.value("message").toString().toStdString());
    check(entries(folder) == std::vector<std::string>{expected.toStdString()},
          "under the name the form showed, and nothing else in the folder");
}

// Text a tag cannot hold is refused; white space around it is not kept.
void textChecks(const QString &scratch)
{
    KeepRecordingController controller;
    controller.setStickRoot(makeStick(scratch));
    const std::string wav = writeWav(scratch, QStringLiteral("text.wav"), 0.5);
    for (const QString &bad : {QStringLiteral("DJ\tKelp"), QStringLiteral("DJ\nKelp"), QStringLiteral("DJ") + QChar(0x7f),
                               QStringLiteral("DJ") + QChar(0x2028) + QStringLiteral("Kelp"),
                               QStringLiteral("DJ") + QChar(0x2029) + QStringLiteral("Kelp"),
                               QStringLiteral("DJ") + QChar(0x1b) + QStringLiteral("Kelp")}) {
        for (const char *field : {"artist", "title", "album", "genre"}) {
            check(!controller.textProblem(QLatin1String(field), bad).isEmpty(),
                  std::string("a control character in the ") + field + " is refused in the form");
            QVariantMap f = form(wav, scratch + QStringLiteral("/text"), QStringLiteral("DJ Kelp"), QStringLiteral("Set"));
            f.insert(QLatin1String(field), bad);
            const bool taken = controller.keep(f);
            check(!taken && controller.errorMessage().contains(QStringLiteral("control character")),
                  std::string("and by keep(), for the ") + field + ": " + controller.errorMessage().toStdString());
        }
    }
    check(controller.textProblem(QStringLiteral("artist"), QStringLiteral("\t DJ Kelp \n")).isEmpty(),
          "white space at either end is taken away, not refused");
    check(controller.textProblem(QStringLiteral("genre"), QString::fromUtf8("Dub \xe2\x80\x94 Techno \xe5\xa4\x9c")).isEmpty(),
          "a dash and CJK are fine");
    check(!controller.busy() && entries(scratch + QStringLiteral("/text")).empty(), "nothing was started or written");
}

// A failure stays in the strip's list while the next job runs and after
// it, until the strip is closed.
void failureStaysWhileTheNextRuns(const QString &scratch)
{
    KeepRecordingController controller;
    controller.setStickRoot(makeStick(scratch));
    const QString folder = scratch + QStringLiteral("/after-failure");
    const std::string missing = (scratch + QStringLiteral("/not there.wav")).toStdString();
    const std::string wav = writeWav(scratch, QStringLiteral("next.wav"), 1);
    QSignalSpy finished(&controller, &KeepRecordingController::finished);
    check(controller.keep(form(missing, folder, QString(), QStringLiteral("Missing"))), "a job for a recording that is gone");
    check(controller.keep(form(wav, folder, QString(), QStringLiteral("Next"))), "and one behind it");
    bool failureShownWhileNextRan = false;
    QObject::connect(&controller, &KeepRecordingController::progressChanged, &controller, [&]() {
        if (controller.busy() && controller.currentRecording() == QStringLiteral("next.wav")) {
            const QVariantList ended = controller.finishedJobs();
            failureShownWhileNextRan = failureShownWhileNextRan
                || (!ended.isEmpty() && ended.first().toMap().value("state").toString() == QStringLiteral("failed"));
        }
    });
    check(waitUntil([&]() { return finished.count() >= 2 && !controller.busy(); }), "both end");
    check(failureShownWhileNextRan, "the failure is listed while the next job runs");
    const QVariantList ended = controller.finishedJobs();
    check(ended.size() == 2 && ended.at(0).toMap().value("state").toString() == QStringLiteral("failed")
              && ended.at(1).toMap().value("state").toString() == QStringLiteral("kept"),
          "and after it: failed, then kept");
}

// The real encoder, and the person cancels right after it placed the
// FLAC: the cancel comes too late, and the FLAC is kept.
class EncoderCancelledAfterPlacing : public seabass::application::RecordingEncoder
{
public:
    explicit EncoderCancelledAfterPlacing(KeepRecordingController *controller) : m_controller(controller) {}
    seabass::application::RecordingEncodeResult encode(const std::string &wav, const std::string &flac,
                                                       const seabass::application::RecordingEncodeOptions &options) override
    {
        return m_real.encode(wav, flac, options);
    }
    seabass::application::RecordingPlaceResult placeVerified(const seabass::application::VerifiedFlac &file,
                                                             const std::string &flac) override
    {
        const auto placed = m_real.placeVerified(file, flac);
        QMetaObject::invokeMethod(m_controller, [c = m_controller]() { c->cancelFor(c->currentStickRoot()); },
                                  Qt::BlockingQueuedConnection);
        return placed;
    }

private:
    KeepRecordingController *m_controller;
    seabass::infrastructure::audio::QtFlacRecordingEncoder m_real;
};

// The real tag writer, and the person cancels while it writes: seen before
// the placing, so nothing is kept.
class TagsCancelledWhileWriting : public seabass::application::RecordingTagWriter
{
public:
    explicit TagsCancelledWhileWriting(KeepRecordingController *controller) : m_controller(controller) {}
    seabass::application::RecordingTagWriteResult tagInPlace(const seabass::application::VerifiedFlac &file,
                                                             const seabass::application::RecordingTags &tags) override
    {
        QMetaObject::invokeMethod(m_controller, [c = m_controller]() { c->cancelFor(c->currentStickRoot()); },
                                  Qt::BlockingQueuedConnection);
        return m_real.tagInPlace(file, tags);
    }

private:
    KeepRecordingController *m_controller;
    seabass::infrastructure::audio::TagLibRecordingTagWriter m_real;
};

void lateCancelAndLeftFiles(const QString &scratch)
{
    const QString stick = makeStick(scratch);
    {
        auto controller = std::make_unique<KeepRecordingController>();
        controller->setStickRoot(stick);
        KeepRecordingController *raw = controller.get();
        KeepRecordingController::setPartsForTesting([raw]() {
            KeepRecordingParts parts;
            parts.files = std::make_unique<seabass::infrastructure::LocalKeepRecordingFiles>();
            parts.encoder = std::make_unique<EncoderCancelledAfterPlacing>(raw);
            parts.covers = std::make_unique<seabass::infrastructure::audio::QtCoverArtPreparer>();
            parts.tags = std::make_unique<seabass::infrastructure::audio::TagLibRecordingTagWriter>();
            return parts;
        });
        const QString folder = scratch + QStringLiteral("/late");
        const std::string wav = writeWav(scratch, QStringLiteral("late.wav"), 0.5);
        QSignalSpy finished(controller.get(), &KeepRecordingController::finished);
        check(controller->keep(form(wav, folder, QString(), QStringLiteral("Late"))), "a job whose cancel comes too late");
        check(waitUntil([&]() { return finished.count() >= 1; }), "it ends");
        const QVariantMap outcome = finished.isEmpty() ? QVariantMap() : finished.first().first().toMap();
        check(outcome.value("state").toString() == QStringLiteral("kept"),
              "a cancel after the FLAC was placed: kept, said so: " + outcome.value("state").toString().toStdString());
        check(entries(folder) == std::vector<std::string>{"Late.flac"}, "and the FLAC stays");
    }
    {
        auto controller = std::make_unique<KeepRecordingController>();
        controller->setStickRoot(stick);
        KeepRecordingController *raw = controller.get();
        KeepRecordingController::setPartsForTesting([raw]() {
            KeepRecordingParts parts;
            parts.files = std::make_unique<seabass::infrastructure::LocalKeepRecordingFiles>();
            parts.encoder = std::make_unique<seabass::infrastructure::audio::QtFlacRecordingEncoder>();
            parts.covers = std::make_unique<seabass::infrastructure::audio::QtCoverArtPreparer>();
            parts.tags = std::make_unique<TagsCancelledWhileWriting>(raw);
            return parts;
        });
        const QString folder = scratch + QStringLiteral("/during-tags");
        const std::string wav = writeWav(scratch, QStringLiteral("during-tags.wav"), 0.5);
        QSignalSpy finished(controller.get(), &KeepRecordingController::finished);
        check(controller->keep(form(wav, folder, QString(), QStringLiteral("During"))), "a job cancelled while tagged");
        check(waitUntil([&]() { return finished.count() >= 1; }), "it ends");
        const QVariantMap outcome = finished.isEmpty() ? QVariantMap() : finished.first().first().toMap();
        check(outcome.value("state").toString() == QStringLiteral("cancelled"),
              "a cancel while the tags are written: cancelled: " + outcome.value("state").toString().toStdString());
        check(entries(folder).empty(), "and nothing is in the folder, no temporary file either");
    }
    {
        // Something else under the FLAC's name after the rename (here the
        // file grown right before it): left as it is, named with its path.
        KeepRecordingController::setPartsForTesting([]() {
            KeepRecordingParts parts;
            parts.files = std::make_unique<seabass::infrastructure::LocalKeepRecordingFiles>();
            auto encoder = std::make_unique<seabass::infrastructure::audio::QtFlacRecordingEncoder>();
            encoder->setBeforeRenameForTests([](const std::string &temp) {
                std::ofstream(seabass::pathFromUtf8(temp), std::ios::binary | std::ios::app) << "more than was checked";
            });
            parts.encoder = std::move(encoder);
            parts.covers = std::make_unique<seabass::infrastructure::audio::QtCoverArtPreparer>();
            parts.tags = std::make_unique<seabass::infrastructure::audio::TagLibRecordingTagWriter>();
            return parts;
        });
        KeepRecordingController controller;
        controller.setStickRoot(stick);
        const QString folder = scratch + QStringLiteral("/left");
        const std::string wav = writeWav(scratch, QStringLiteral("left.wav"), 0.5);
        QSignalSpy finished(&controller, &KeepRecordingController::finished);
        check(controller.keep(form(wav, folder, QString(), QStringLiteral("Left"))), "a job whose FLAC is another size after the rename");
        check(waitUntil([&]() { return finished.count() >= 1; }), "it ends");
        const QVariantMap outcome = finished.isEmpty() ? QVariantMap() : finished.first().first().toMap();
        const QString left = QDir::cleanPath(QDir::fromNativeSeparators(outcome.value("leftBehind").toString()));
        check(outcome.value("state").toString() == QStringLiteral("failed"), "not kept");
        check(left == QDir::cleanPath(folder + QStringLiteral("/Left.flac")),
              "the file left is named with its path: " + left.toStdString());
        check(outcome.value("message").toString().contains(QDir::toNativeSeparators(folder)),
              "the message says where: " + outcome.value("message").toString().toStdString());
        check(entries(folder) == std::vector<std::string>{"Left.flac"}, "and it is left where it is");
    }
    KeepRecordingController::setPartsForTesting({});
}

}  // namespace

// A recording whose header was not completed is kept only with the choice
// (the reason says the length the file holds, through the real encoder); the
// kept copy counts in the delete confirmation only while its FLAC is there
// as it was kept; a recording is known by its path, size and time; and
// the records are forgotten when the stick changes.
void keptCopiesAndIdentity(const QString &scratch)
{
    const QString stick = makeStick(scratch);
    const QString folder = scratch + QStringLiteral("/kept");
    const QString sessions = stick + QStringLiteral("/Sessions");
    const std::string wav = QDir(sessions).filePath(QStringLiteral("Unfinished.wav")).toStdString();
    recording_test::WavSpec spec;
    spec.frames = 44100;
    spec.declaredDataBytes = 0xFFFFFFFFU;
    recording_test::writeWav(wav, spec);
    const QString wavPath = QString::fromStdString(wav);

    KeepRecordingController controller;
    controller.setStickRoot(stick);
    QSignalSpy ended(&controller, &KeepRecordingController::finished);
    const auto runJob = [&](const QVariantMap &f) {
        const int before = static_cast<int>(ended.count());
        if (!controller.keep(f)) {
            return QVariantMap{{"refused", controller.errorMessage()}};
        }
        waitUntil([&]() { return ended.count() > before; });
        return ended.count() > before ? ended.last().first().toMap() : QVariantMap{};
    };

    // Without the choice: not kept, saying why.
    const QVariantMap refused = runJob(form(wav, folder, QStringLiteral("DJ Kelp"), QStringLiteral("Unfinished")));
    check(refused.value("state").toString() == QStringLiteral("failed")
              && refused.value("message").toString().contains(
                  QStringLiteral("Its header was not completed: it does not say how long the audio is, and the "
                                 "file holds 0:01 of audio.")),
          "an unfinished recording without the choice: not kept: " + refused.value("message").toString().toStdString());
    // With it: kept, the notes say so.
    QVariantMap chosen = form(wav, folder, QStringLiteral("DJ Kelp"), QStringLiteral("Unfinished"));
    chosen.insert("keepUnfinished", true);
    const QVariantMap kept = runJob(chosen);
    check(kept.value("state").toString() == QStringLiteral("kept") && kept.value("unfinished").toBool()
              && kept.value("keptSeconds").toDouble() > 0.99 && kept.value("keptSeconds").toDouble() < 1.01,
          "an unfinished recording with the choice: kept, with how much");
    check(kept.value("notes").toStringList().join(QLatin1Char(' ')).contains(
              QStringLiteral("did not say how long the audio is, so the file was read to its end: it held "
                             "0:01 of audio. The FLAC holds 0:01")),
          "and its notes say the header was not completed and the file was read to its end: "
              + kept.value("notes").toStringList().join(QLatin1Char(' ')).toStdString());
    const QString flac = kept.value("flacPath").toString();
    check(controller.keptFlacFor(stick, wavPath) == flac && !controller.keptFlacChanged(stick, wavPath),
          "the FLAC as saved is the one Browse offers to add");

    // Kept once, then a second keep that fails: the first copy still counts.
    const QVariantMap second = runJob(form(wav, folder, QStringLiteral("DJ Kelp"), QStringLiteral("Again")));
    check(second.value("state").toString() == QStringLiteral("failed"), "a second keep without the choice fails");
    QVariantMap notes = controller.keepNotesFor(stick, {wavPath});
    check(notes.value("kept").toList().size() == 1 && notes.value("notKept").toList().isEmpty()
              && notes.value("keptGone").toList().isEmpty(),
          "kept once, then a failed keep: still kept, its FLAC is there");

    // The FLAC changed: not counted as kept, said apart.
    {
        QFile file(flac);
        file.open(QIODevice::Append);
        file.write("x");
    }
    notes = controller.keepNotesFor(stick, {wavPath});
    check(notes.value("kept").toList().isEmpty() && notes.value("keptGone").toList().size() == 1,
          "a kept FLAC whose size changed: not counted as kept, said as gone");
    check(controller.keptFlacFor(stick, wavPath).isEmpty() && controller.keptFlacChanged(stick, wavPath),
          "a FLAC changed since it was saved: not offered to add, said to be another file");
    // The FLAC gone.
    QFile::remove(flac);
    notes = controller.keepNotesFor(stick, {wavPath});
    check(notes.value("kept").toList().isEmpty() && notes.value("keptGone").toList().size() == 1
              && notes.value("keptGone").toList().first().toMap().value("fileName").toString()
                  == QFileInfo(flac).fileName(),
          "a kept FLAC that is gone: not counted as kept, said with its name");
    check(controller.keptFlacFor(stick, wavPath).isEmpty() && !controller.keptFlacChanged(stick, wavPath),
          "a FLAC that is gone: not offered, and not another file");

    // Another recording under the same path: not the one kept.
    recording_test::WavSpec other;
    other.frames = 22050;
    recording_test::writeWav(wav, other);
    check(controller.outcomeFor(stick, wavPath).isEmpty(), "another file under the same path has no outcome");
    notes = controller.keepNotesFor(stick, {wavPath});
    check(notes.value("kept").toList().isEmpty() && notes.value("keptGone").toList().isEmpty()
              && notes.value("notKept").toList().isEmpty(),
          "and is named nowhere in the delete confirmation");

    // The form for another stick: the records stay, each of its stick.
    const QString wav2 = QString::fromStdString(writeWav(sessions, QStringLiteral("Plain.wav"), 0.5));
    const QVariantMap plain = runJob(form(wav2.toStdString(), folder, QStringLiteral("DJ Kelp"), QStringLiteral("Plain")));
    check(plain.value("state").toString() == QStringLiteral("kept"), "a plain recording is kept");
    check(!controller.outcomeFor(stick, wav2).isEmpty(), "and has its outcome");
    const QString otherStick = scratch + QStringLiteral("/OTHER");
    controller.setStickRoot(otherStick);
    check(controller.outcomeFor(stick, wav2).value("state").toString() == QStringLiteral("kept")
              && controller.keepNotesFor(stick, {wav2}).value("kept").toList().size() == 1,
          "the form for another stick: this stick's outcomes and kept copies stay");
    check(controller.outcomeFor(otherStick, wav2).isEmpty() && controller.keepNotesFor(otherStick, {wav2}).value("kept").toList().isEmpty(),
          "and are not the other stick's");
}

// One controller for every stick's page, two sticks: the form for one
// stick leaves the other's records as they were, each stick is asked
// about its own work, a cancel of a stick (it was taken out) leaves the
// other's jobs waiting, the queue stays one job at a time across sticks,
// and a delete on one stick holds off keeps of that stick only.
void twoSticks(const QString &scratch)
{
    const QString stickA = makeStick(scratch);
    const QString stickB = scratch + QStringLiteral("/STICK B");
    QDir().mkpath(stickB + QStringLiteral("/Sessions"));
    const QString folder = scratch + QStringLiteral("/kept");
    const QString a1 = QString::fromStdString(writeWav(stickA + QStringLiteral("/Sessions"), QStringLiteral("A1.wav"), 0.5));
    const QString a2 = QString::fromStdString(writeWav(stickA + QStringLiteral("/Sessions"), QStringLiteral("A2.wav"), 0.5));
    const QString a3 = QString::fromStdString(writeWav(stickA + QStringLiteral("/Sessions"), QStringLiteral("A3.wav"), 0.5));
    const QString b1 = QString::fromStdString(writeWav(stickB + QStringLiteral("/Sessions"), QStringLiteral("B1.wav"), 240));
    const QString b2 = QString::fromStdString(writeWav(stickB + QStringLiteral("/Sessions"), QStringLiteral("B2.wav"), 0.5));
    const QString b3 = QString::fromStdString(writeWav(stickB + QStringLiteral("/Sessions"), QStringLiteral("B3.wav"), 0.5));
    const auto formFor = [&folder](const QString &wav, const QString &title) {
        return form(wav.toStdString(), folder, QStringLiteral("DJ Kelp"), title);
    };
    const auto stateOf = [](const QVariantList &jobs) {
        QStringList states;
        for (const QVariant &job : jobs) {
            states << job.toMap().value("state").toString() + QLatin1Char(' ') + job.toMap().value("recording").toString();
        }
        return states;
    };

    KeepRecordingController controller;
    QSignalSpy finished(&controller, &KeepRecordingController::finished);
    controller.setStickRoot(stickA);
    check(controller.keep(formFor(a1, QStringLiteral("A1"))), "stick A: a first recording taken");
    check(waitUntil([&]() { return finished.count() >= 1; }), "and kept");

    // The next job of A holds in the tag writer until the test lets go.
    Latch latch;
    KeepRecordingController::setPartsForTesting([&latch]() {
        KeepRecordingParts parts;
        parts.files = std::make_unique<seabass::infrastructure::LocalKeepRecordingFiles>();
        parts.encoder = std::make_unique<seabass::infrastructure::audio::QtFlacRecordingEncoder>();
        parts.covers = std::make_unique<seabass::infrastructure::audio::QtCoverArtPreparer>();
        parts.tags = std::make_unique<HeldTags>(&latch);
        return parts;
    });
    check(controller.keep(formFor(a2, QStringLiteral("A2"))), "stick A: a second recording taken");
    check(waitUntil([&]() { return latch.entered(); }), "and it runs, held in the tag writer");
    KeepRecordingController::setPartsForTesting({});

    // Stick B's page opens: the form is for B now.
    controller.setStickRoot(stickB);
    check(controller.outcomeFor(stickA, a1).value("state").toString() == QStringLiteral("kept")
              && controller.keepNotesFor(stickA, {a1}).value("kept").toList().size() == 1,
          "the form for stick B leaves what stick A kept known");
    check(stateOf(controller.finishedJobsFor(stickA)) == QStringList{QStringLiteral("kept A1.wav")},
          "and stick A's strip still lists its ended job");
    check(controller.finishedJobsFor(stickB).isEmpty() && controller.queuedFor(stickB).isEmpty()
              && !controller.hasWorkFor(stickB),
          "stick B's strip has nothing of stick A's work");
    check(controller.outcomeFor(stickB, a1).isEmpty() && controller.keepNotesFor(stickB, {a1}).value("kept").toList().isEmpty(),
          "stick A's records are not stick B's");
    check(controller.hasWorkFor(stickA) && controller.busy() && controller.currentStickRoot() == QDir::cleanPath(stickA),
          "stick A has work: its job is the one in hand");
    check(controller.hasWorkFor(stickA + QStringLiteral("/")), "a stick is the same stick spelt with a trailing slash");

    check(controller.keep(formFor(b1, QStringLiteral("B1"))) && controller.keep(formFor(b2, QStringLiteral("B2"))),
          "stick B: two recordings taken while stick A's runs");
    check(controller.queuedFor(stickB).size() == 2 && controller.queuedFor(stickA).isEmpty()
              && controller.hasWorkFor(stickB) && controller.currentStickRoot() == QDir::cleanPath(stickA),
          "they wait behind stick A's: one job at a time across sticks");
    check(controller.outcomeFor(stickB, b1).value("state").toString() == QStringLiteral("queued"),
          "B1's row says it waits");

    // Stick A is taken out: its work stops, stick B's waits on.
    controller.cancelFor(stickA);
    check(controller.cancelRequested(), "stick A's job is asked to stop");
    check(controller.queuedFor(stickB).size() == 2 && controller.finishedJobsFor(stickB).isEmpty(),
          "stick B's jobs still wait, none dropped");
    latch.release();
    check(waitUntil([&]() { return controller.busy() && controller.currentRecording() == QStringLiteral("B1.wav"); }),
          "stick A's job ends, and stick B's first starts");
    check(stateOf(controller.finishedJobsFor(stickA))
              == (QStringList{QStringLiteral("kept A1.wav"), QStringLiteral("cancelled A2.wav")}),
          "stick A's strip: kept, then stopped: " + stateOf(controller.finishedJobsFor(stickA)).join(", ").toStdString());

    // A delete on stick B: nothing more is kept from B meanwhile, A is free.
    controller.setDeleteRunning(stickB, true);
    check(!controller.keep(formFor(b3, QStringLiteral("B3"))) && controller.errorMessage().contains(QStringLiteral("deleted")),
          "while stick B's recordings are deleted, none of B is kept: " + controller.errorMessage().toStdString());
    controller.setStickRoot(stickA);
    check(controller.keep(formFor(a3, QStringLiteral("A3"))), "and one of stick A is taken");
    controller.setDeleteRunning(stickB, false);
    check(controller.queuedFor(stickA).size() == 1 && controller.queuedFor(stickB).size() == 1,
          "it waits behind stick B's, with B2");

    // Stick B is taken out: its job in hand stops and its waiting one is
    // dropped; stick A's waits on and runs.
    controller.cancelFor(stickB);
    check(controller.queuedFor(stickB).isEmpty() && controller.queuedFor(stickA).size() == 1,
          "stick B's waiting job is dropped, stick A's still waits");
    check(waitUntil([&]() { return !controller.busy() && controller.queued().isEmpty(); }), "everything ends");
    check(stateOf(controller.finishedJobsFor(stickB))
              == (QStringList{QStringLiteral("dropped B2.wav"), QStringLiteral("cancelled B1.wav")}),
          "stick B's strip: dropped, then stopped: " + stateOf(controller.finishedJobsFor(stickB)).join(", ").toStdString());
    check(stateOf(controller.finishedJobsFor(stickA)).last() == QStringLiteral("kept A3.wav"),
          "stick A's last job is kept");

    // Each strip closed on its own.
    controller.dismissFinishedFor(stickA);
    check(controller.finishedJobsFor(stickA).isEmpty() && controller.finishedJobsFor(stickB).size() == 2,
          "closing stick A's strip leaves stick B's");
    check(controller.outcomeFor(stickA, a3).value("state").toString() == QStringLiteral("kept"),
          "and the rows still say what happened");
}

int main(int argc, char **argv)
{
    // The settings store, a scratch file on every platform: the
    // environment for Linux, an INI file at a path of the test's own for
    // macOS and Windows, where the environment is not asked (see
    // open_folder_test.cpp for why both). Proven before anything is
    // written, through the call the controller makes.
    const fs::path settingsHome = seabass::testing::scratchRoot() / "keep_recording_controller_test" / "config";
    seabass::testing::sandboxSettings(settingsHome);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, seabass::gui::pathToQString(settingsHome));
    QGuiApplication app(argc, argv);
    {
        const QSettings probe = seabass::gui::openSeabassSettings();
        const QString where = QDir::cleanPath(QDir::fromNativeSeparators(probe.fileName()));
        if (!where.startsWith(QDir::cleanPath(seabass::gui::pathToQString(settingsHome)) + QLatin1Char('/'))) {
            std::cerr << "FAILED: the settings store is not the scratch one: " << where.toStdString() << "\n";
            return 1;
        }
    }
    if (QCoreApplication::arguments().size() < 2) {
        std::cerr << "FAILED: give a scratch folder\n";
        return 1;
    }
    if (!seabass::infrastructure::audio::QtFlacRecordingEncoder::encodingAvailable()) {
        std::cerr << "FAILED: this Qt cannot encode FLAC, so nothing here can be checked\n";
        return 1;
    }
    const QString scratchRoot = QCoreApplication::arguments().at(1);
    QDir().mkpath(scratchRoot);
    QTemporaryDir scratch(scratchRoot + QStringLiteral("/keep-recording-XXXXXX"));
    if (!scratch.isValid()) {
        std::cerr << "FAILED: no scratch directory\n";
        return 1;
    }
    const QString dir = QDir(scratch.path()).absolutePath();

    suggestionsAndChecks(dir + QStringLiteral("/checks"));
    twoJobsQueued(dir + QStringLiteral("/queued"));
    cancelInTheMiddle(dir + QStringLiteral("/cancel"));
    destroyedMidEncoding(dir + QStringLiteral("/destroy"));
    destroyedMidTagging(dir + QStringLiteral("/destroy-tags"));
    destroyedMidCoverPreview(dir + QStringLiteral("/destroy-cover"));
    folderOnTheStick(dir + QStringLiteral("/on-stick"));
    longNames(dir + QStringLiteral("/long-names"));
    textChecks(dir + QStringLiteral("/text"));
    failureStaysWhileTheNextRuns(dir + QStringLiteral("/failure-stays"));
    lateCancelAndLeftFiles(dir + QStringLiteral("/late-cancel"));
    keptCopiesAndIdentity(dir + QStringLiteral("/kept-copies"));
    twoSticks(dir + QStringLiteral("/two-sticks"));

    if (failures > 0) {
        std::cout << "keep_recording_controller_test: " << failures << " FAILED\n";
        return 1;
    }
    std::cout << "keep_recording_controller_test: all passed\n";
    return 0;
}
