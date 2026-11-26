// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// KeepRecordingController (gui/keep_recording_controller.hpp) with the real
// encoder, cover preparer and tag writer on short WAVs this test writes
// (recording_test_support.hpp: tones and noise, no real audio) into a
// scratch folder: the form's suggestions and checks, a tagged FLAC with
// its cover, progress that never goes back and ends full, a second job
// that waits for the first, a cancel in the middle, and the controller
// destroyed in the middle of a job, in the encoder and in the tag writer.
// What is in the target folder afterwards is read from the disk here.

#include <QDir>
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

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/xiphcomment.h>

#include "gui/keep_recording_controller.hpp"
#include "infrastructure/audio/qt_cover_art_preparer.hpp"
#include "infrastructure/audio/qt_flac_recording_encoder.hpp"
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

std::string writeWav(const QString &dir, const QString &name, double seconds)
{
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

    check(controller.fileNameFor(QStringLiteral(" AC/DC "), QStringLiteral("Live?")) == QStringLiteral("AC_DC - Live_.flac"),
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

    check(controller.keep(form(wav1, folder, QStringLiteral("DJ Kelp"), QStringLiteral("Late Set"))), "first job taken");
    check(controller.busy(), "and running");
    controller.pickCover(QString());
    check(controller.keep(form(wav2, folder, QStringLiteral("DJ Kelp"), QStringLiteral("Early Set"))), "second job taken");
    check(controller.queued().size() == 1, "the second waits in the queue");
    check(controller.outcomeFor(QString::fromStdString(wav2)).value("state").toString() == QStringLiteral("queued"),
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

    const QVariantMap kept = controller.outcomeFor(QString::fromStdString(wav1));
    check(kept.value("state").toString() == QStringLiteral("kept"), "the row says kept");
    const QString flac1 = folder + QStringLiteral("/DJ Kelp - Late Set.flac");
    check(QDir::cleanPath(QDir::fromNativeSeparators(kept.value("flacPath").toString())) == QDir::cleanPath(flac1),
          "and where: " + kept.value("flacPath").toString().toStdString());
    check(entries(folder) == (std::vector<std::string>{"DJ Kelp - Early Set.flac", "DJ Kelp - Late Set.flac"}),
          "the folder holds the two FLACs and nothing else");
    check(controller.lastFinished().value("recording").toString() == QStringLiteral("Session-0002.wav"),
          "the strip shows the job that ended last");

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
    KeepRecordingController controller;
    bool asked = false;
    QObject::connect(&controller, &KeepRecordingController::progressChanged, &controller, [&]() {
        if (!asked && controller.progress() >= 0.2) {
            asked = true;
            controller.cancel();
        }
    });
    QSignalSpy finished(&controller, &KeepRecordingController::finished);
    check(controller.keep(form(wav, folder, QString(), QStringLiteral("Long"))), "a long job taken");
    check(controller.keep(form(waiting, folder, QString(), QStringLiteral("Waiting"))), "and one behind it");
    check(waitUntil([&]() { return finished.count() >= 1 && !controller.busy(); }), "the cancelled job ends");
    check(asked, "cancelled in the middle, at a fifth");
    check(finished.count() == 1, "only the job in hand ends: the one that waited was dropped");
    const QVariantMap outcome = finished.isEmpty() ? QVariantMap() : finished.first().first().toMap();
    check(outcome.value("state").toString() == QStringLiteral("cancelled"), "it says cancelled");
    check(controller.queued().isEmpty(), "nothing waits after a cancel");
    check(entries(folder).empty(), "no file of it in the folder, no temporary file either");
    check(controller.outcomeFor(QString::fromStdString(wav)).value("state").toString().isEmpty(),
          "the row says nothing: the person asked for it");
    check(fs::file_size(seabass::pathFromUtf8(wav)) == 44 + 44100ULL * 240 * 4, "the recording is as it was");
}

void destroyedMidEncoding(const QString &scratch)
{
    const QString folder = scratch + QStringLiteral("/destroyed");
    const std::string wav = writeWav(scratch, QStringLiteral("destroyed.wav"), 240);
    auto controller = std::make_unique<KeepRecordingController>();
    check(controller->keep(form(wav, folder, QString(), QStringLiteral("Gone"))), "a job taken, to be left");
    check(waitUntil([&]() { return controller->progress() >= 0.1; }), "it is under way");
    QElapsedTimer timer;
    timer.start();
    controller.reset();
    const qint64 ms = timer.elapsed();
    check(ms < 1500, "the controller goes within a second and a half: " + std::to_string(ms) + " ms");
    QThreadPool::globalInstance()->waitForDone();
    check(entries(folder).empty(), "no file left in the folder");
}

// A tag writer that takes its time: the step the destructor cannot
// interrupt.
class SlowTags : public seabass::application::RecordingTagWriter
{
public:
    seabass::application::RecordingTagWriteResult write(const std::string &, const seabass::application::RecordingTags &) override
    {
        QThread::msleep(2500);
        return {true, {}};
    }
};

void destroyedMidTagging(const QString &scratch)
{
    const QString folder = scratch + QStringLiteral("/destroyed-tagging");
    const std::string wav = writeWav(scratch, QStringLiteral("tagging.wav"), 2);
    KeepRecordingController::setPartsForTesting([]() {
        KeepRecordingParts parts;
        parts.files = std::make_unique<seabass::infrastructure::LocalKeepRecordingFiles>();
        parts.encoder = std::make_unique<seabass::infrastructure::audio::QtFlacRecordingEncoder>();
        parts.covers = std::make_unique<seabass::infrastructure::audio::QtCoverArtPreparer>();
        parts.tags = std::make_unique<SlowTags>();
        return parts;
    });
    auto controller = std::make_unique<KeepRecordingController>();
    check(controller->keep(form(wav, folder, QString(), QStringLiteral("Slow"))), "a job taken, to be left while tagged");
    check(waitUntil([&]() { return controller->progressStep() == QStringLiteral("tags"); }), "it reaches the tags");
    QElapsedTimer timer;
    timer.start();
    controller.reset();
    const qint64 ms = timer.elapsed();
    check(ms < 1500, "the controller goes within a second and a half while the tags are written: " + std::to_string(ms) + " ms");
    QThreadPool::globalInstance()->waitForDone();
    check(entries(folder).empty(), "the worker took its FLAC away when the tag writer returned");
    KeepRecordingController::setPartsForTesting({});
}

// A cover preparer that takes its time, as a huge picture on a slow disk.
class SlowCovers : public seabass::application::CoverArtPreparer
{
public:
    seabass::application::CoverArtResult prepare(const std::string &) override
    {
        QThread::msleep(3000);
        return {std::nullopt, "too slow"};
    }
};

// Leaving the page while a cover preview is under way waits for it about
// a second, not until it ends.
void destroyedMidCoverPreview(const QString &scratch)
{
    QDir().mkpath(scratch);
    const QString picture = scratch + QStringLiteral("/slow.png");
    QImage(64, 64, QImage::Format_RGB32).save(picture);
    KeepRecordingController::setPartsForTesting([]() {
        KeepRecordingParts parts;
        parts.files = std::make_unique<seabass::infrastructure::LocalKeepRecordingFiles>();
        parts.encoder = std::make_unique<seabass::infrastructure::audio::QtFlacRecordingEncoder>();
        parts.covers = std::make_unique<SlowCovers>();
        parts.tags = std::make_unique<SlowTags>();
        return parts;
    });
    auto controller = std::make_unique<KeepRecordingController>();
    controller->pickCover(picture);
    check(controller->coverPreparing(), "a cover preview is under way");
    QElapsedTimer timer;
    timer.start();
    controller.reset();
    const qint64 ms = timer.elapsed();
    check(ms < 1500, "the controller goes within a second and a half while a cover is prepared: " + std::to_string(ms)
                         + " ms");
    QThreadPool::globalInstance()->waitForDone();
    KeepRecordingController::setPartsForTesting({});
}

}  // namespace

int main(int argc, char **argv)
{
    const fs::path settingsHome = seabass::testing::scratchRoot() / "keep_recording_controller_test" / "config";
    seabass::testing::sandboxSettings(settingsHome);
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SeabassTest"));
    QCoreApplication::setApplicationName(QStringLiteral("keep_recording_controller_test"));
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

    if (failures > 0) {
        std::cout << "keep_recording_controller_test: " << failures << " FAILED\n";
        return 1;
    }
    std::cout << "keep_recording_controller_test: all passed\n";
    return 0;
}
