// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// RecordingsController (gui/recordings_controller.hpp) against a planted
// stick in scratch: the hub's quick summary, the listing with durations,
// a delete run end to end through the worker, the summary it reports,
// and the stick's operation log. What the summary claims is
// checked against the filesystem here.

#include <QCoreApplication>
#include <QSignalSpy>
#include <QStringList>

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

#include "gui/keep_recording_controller.hpp"
#include "gui/qt_path.hpp"
#include "gui/recordings_controller.hpp"
#include "infrastructure/local/local_keep_recording_files.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "nothing_else_running.hpp"
#include "scratch_path.hpp"

using seabass::gui::KeepRecordingController;
using seabass::gui::KeepRecordingParts;
using seabass::gui::RecordingsController;
using seabass::gui::pathToQString;
namespace fs = std::filesystem;

namespace
{

std::string wav(std::uint32_t frames, char fill)
{
    const std::uint32_t data = frames * 4;
    std::string out = "RIFF";
    auto put32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
        }
    };
    auto put16 = [&out](std::uint16_t v) {
        out.push_back(static_cast<char>(v & 0xff));
        out.push_back(static_cast<char>(v >> 8));
    };
    put32(36 + data);
    out += "WAVEfmt ";
    put32(16);
    put16(1);
    put16(2);
    put32(44100);
    put32(44100 * 4);
    put16(4);
    put16(16);
    out += "data";
    put32(data);
    return out + std::string(data, fill);
}

void write(const fs::path &path, const std::string &bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}

std::string read(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool waitFor(QSignalSpy &spy, int count)
{
    for (int i = 0; i < 300 && spy.count() < count; ++i) {
        spy.wait(100);
    }
    return spy.count() >= count;
}

// An encoder held until the test lets it go (or the job is cancelled),
// which then writes a stand-in FLAC: a keep that runs as long as the test
// needs it to.
class HeldEncoder : public seabass::application::RecordingEncoder
{
public:
    explicit HeldEncoder(std::atomic<bool> *release) : m_release(release) {}
    seabass::application::RecordingEncodeResult encode(const std::string &, const std::string &flacPath,
                                                       const seabass::application::RecordingEncodeOptions &options) override
    {
        seabass::application::RecordingEncodeResult result;
        for (int i = 0; i < 30000 && !m_release->load(); ++i) {
            if (options.cancel.cancelled()) {
                result.cancelled = true;
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        write(seabass::pathFromUtf8(flacPath), "fLaC stand-in");
        result.encoded = true;
        return result;
    }

private:
    std::atomic<bool> *m_release;
};

class NoCovers : public seabass::application::CoverArtPreparer
{
public:
    seabass::application::CoverArtResult prepare(const std::string &) override { return {std::nullopt, "no covers here"}; }
};

class PassingTags : public seabass::application::RecordingTagWriter
{
public:
    seabass::application::RecordingTagWriteResult write(const std::string &, const seabass::application::RecordingTags &) override
    {
        return {true, {}};
    }
};

}  // namespace

int main(int argc, char **argv)
{
    seabass::tests::assumeNothingElseRunning();
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path scratch = seabass::testing::scratchRoot() / "recordings_controller_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    seabass::testing::sandboxSeabassHome(scratch / "home");
    seabass::testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);

    const fs::path stick = scratch / "STICK";
    const fs::path engine = stick / "Engine Library";
    fs::create_directories(engine / "Database2");
    const std::string one = wav(44100 * 3, 'a');  // three seconds
    const std::string two = wav(44100 * 2, 'b');
    write(stick / "Sessions" / "Session-0001.wav", one);
    write(stick / "PIONEER REC" / "REC001.WAV", two);
    write(stick / "Sessions" / "notes.txt", "not mine");
    write(stick / "Contents" / "Track.wav", wav(100, 'c'));

    RecordingsController controller;

    // The hub's summary: counts and sources, no durations.
    const QVariantMap summary = controller.summarize(QString(), pathToQString(engine));
    assert(summary.value("count").toInt() == 2);
    assert(static_cast<std::uint64_t>(summary.value("bytes").toDouble()) == one.size() + two.size());
    assert(summary.value("sources").toStringList() == (QStringList{"engine", "pioneer"}));
    assert(summary.value("unreadable").toBool() == false);
    std::cout << "ok: summary counts two recordings from Engine OS and a Pioneer deck\n";

    // The listing, on the worker. Nothing starts ticked.
    QSignalSpy listed(&controller, &RecordingsController::listingChanged);
    controller.load(QStringLiteral("STICK"), QString(), pathToQString(engine));
    assert(waitFor(listed, 1));
    assert(controller.listed());
    assert(controller.recordingCount() == 2);
    assert(controller.selectedCount() == 0);
    assert(controller.selectedBytes() == 0);
    assert(static_cast<std::uint64_t>(controller.totalBytes()) == one.size() + two.size());
    assert(controller.stickTotalBytes() > 0);
    assert(controller.stickFreeBytes() > 0);
    assert(controller.leftAlone().size() == 1);
    auto *model = controller.recordings();
#if defined(SEABASS_HAVE_TAGLIB)
    // TagLib is linked into this test exactly when the app has it.
    bool sawThreeSeconds = false;
    for (int i = 0; i < model->rowCount(); ++i) {
        const double seconds = model->data(model->index(i), seabass::gui::RecordingListModel::DurationSecondsRole).toDouble();
        sawThreeSeconds = sawThreeSeconds || (seconds > 2.9 && seconds < 3.1);
    }
    assert(sawThreeSeconds);
#endif
    std::cout << "ok: the listing names both, ticks none, leaves notes.txt alone, and reads a length\n";

    // Nothing ticked: nothing to delete, nothing starts.
    controller.deleteSelected();
    assert(!controller.working());

    // Tick one; the other must survive.
    int sessionRow = -1;
    for (int i = 0; i < model->rowCount(); ++i) {
        if (model->data(model->index(i), seabass::gui::RecordingListModel::FileNameRole).toString() == "Session-0001.wav") {
            sessionRow = i;
        }
    }
    assert(sessionRow >= 0);
    controller.setIncluded(sessionRow, true);
    assert(controller.selectedCount() == 1);
    assert(static_cast<std::uint64_t>(controller.selectedBytes()) == one.size());

    QSignalSpy finished(&controller, &RecordingsController::finished);
    QSignalSpy progress(&controller, &RecordingsController::progressChanged);
    controller.deleteSelected();
    assert(controller.working());
    assert(waitFor(finished, 1));
    const QVariantMap result = finished.takeFirst().at(0).toMap();
    assert(result.value("error").toString().isEmpty());
    assert(result.value("written").toInt() == 1);
    assert(result.value("total").toInt() == 1);
    assert(result.value("unit").toString() == "recordings");
    assert(result.value("verb").toString() == "deleted");
    assert(result.value("cancelled").toBool() == false);
    assert(result.value("warning").toString().isEmpty());
    assert(progress.count() > 1);
    // The claims, asked of the filesystem.
    assert(!fs::exists(stick / "Sessions" / "Session-0001.wav"));
    assert(read(stick / "PIONEER REC" / "REC001.WAV") == two);  // not ticked, not touched
    assert(fs::exists(stick / "Sessions" / "notes.txt"));
    assert(fs::exists(stick / "Contents" / "Track.wav"));
    assert(fs::is_directory(stick / "Sessions"));
    // The stick's own log says what happened.
    const std::string log = read(seabass::infrastructure::paths::stickOperationLog(stick));
    assert(log.find("recordings: permanently deleted") != std::string::npos);
    assert(log.find("Session-0001.wav") != std::string::npos);
    std::cout << "ok: the ticked recording is deleted, the other stays; the summary matches the stick\n";

    // After the run the list is read again from the stick.
    assert(waitFor(listed, 2));
    assert(!controller.busy());
    assert(controller.recordingCount() == 1);
    std::cout << "ok: the list is read again after the run\n";

    // Cancelled mid-run: the worker is held inside the first delete until
    // the cancel has been asked for, so the answer does not depend on
    // timing. The file in flight finishes, the rest stay.
    {
        for (const char *name : {"C1.wav", "C2.wav", "C3.wav"}) {
            write(stick / "Sessions" / name, wav(1000, name[1]));
        }
        std::atomic<bool> release{false};
        seabass::application::RecordingDeleteHooks hooks;
        hooks.beforeDelete = [&release](const fs::path &) {
            for (int i = 0; i < 5000 && !release.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        RecordingsController::setDeleteHooksForTesting(hooks);
        const int listedBefore = listed.count();
        controller.load(QStringLiteral("STICK"), QString(), pathToQString(engine));
        assert(waitFor(listed, listedBefore + 1));
        assert(controller.recordingCount() == 4);
        controller.setAllIncluded(true);
        assert(controller.selectedCount() == 4);
        bool asked = false;
        auto cancelOnFirstWord = [&]() {
            // The first word from the worker, not the reset the start announces.
            if (!asked && controller.working() && !controller.currentItem().isEmpty()) {
                asked = true;
                controller.cancel();
                release = true;
            }
        };
        const QMetaObject::Connection watch =
            QObject::connect(&controller, &RecordingsController::progressChanged, &controller, cancelOnFirstWord);
        controller.deleteSelected();
        assert(waitFor(finished, 1));
        RecordingsController::setDeleteHooksForTesting({});
        QObject::disconnect(watch);  // it holds references into this block
        const QVariantMap stopped = finished.takeFirst().at(0).toMap();
        assert(asked);
        assert(stopped.value("cancelled").toBool());
        assert(stopped.value("written").toInt() == 1);
        assert(stopped.value("total").toInt() == 4);
        int onStick = 0;
        for (const char *name : {"C1.wav", "C2.wav", "C3.wav"}) {
            onStick += fs::exists(stick / "Sessions" / name) ? 1 : 0;
        }
        onStick += fs::exists(stick / "PIONEER REC" / "REC001.WAV") ? 1 : 0;
        assert(onStick == 3);
        std::cout << "ok: cancelled mid-run, 1 of 4 deleted and the rest left on the stick\n";
    }

    // What could not be deleted is named: a recording taken off the stick
    // after it was listed.
    {
        for (int i = 0; i < 300 && controller.busy(); ++i) {
            listed.wait(100);
        }
        const int listedBefore = listed.count();
        controller.load(QStringLiteral("STICK"), QString(), pathToQString(engine));
        assert(waitFor(listed, listedBefore + 1));
        assert(controller.recordingCount() == 3);
        controller.setAllIncluded(true);
        fs::remove(stick / "Sessions" / "C3.wav");
        controller.deleteSelected();
        assert(waitFor(finished, 1));
        const QVariantMap partly = finished.takeFirst().at(0).toMap();
        assert(partly.value("written").toInt() == 2);
        const QString warning = partly.value("warning").toString();
        assert(warning.contains(QStringLiteral("not deleted")));
        assert(warning.contains(QStringLiteral("C3.wav")));
        std::cout << "ok: what could not be deleted is named in the summary\n";
    }

    // Keep this recording beside the list, wired in C++: the keep
    // controller learns the stick, nothing is deleted while it has work,
    // and nothing is kept while a delete runs.
    {
        for (int i = 0; i < 300 && controller.busy(); ++i) {
            listed.wait(100);
        }
        write(stick / "Sessions" / "K1.wav", wav(1000, 'k'));
        write(stick / "Sessions" / "K2.wav", wav(1000, 'l'));
        std::atomic<bool> release{false};
        KeepRecordingController::setPartsForTesting([&release]() {
            KeepRecordingParts parts;
            parts.files = std::make_unique<seabass::infrastructure::LocalKeepRecordingFiles>();
            parts.encoder = std::make_unique<HeldEncoder>(&release);
            parts.covers = std::make_unique<NoCovers>();
            parts.tags = std::make_unique<PassingTags>();
            return parts;
        });
        // Wired before its first listing, as Main.qml wires them: the
        // stick is told when the list is read.
        KeepRecordingController keep;
        RecordingsController wired;
        QSignalSpy wiredListed(&wired, &RecordingsController::listingChanged);
        wired.setKeepController(&keep);
        assert(keep.stickRoot().isEmpty());
        const int listedBefore = wiredListed.count();
        wired.load(QStringLiteral("STICK"), QString(), pathToQString(engine));
        assert(waitFor(wiredListed, listedBefore + 1));
        assert(fs::equivalent(seabass::gui::pathFromQString(keep.stickRoot()), stick));
        assert(keep.folderProblem(pathToQString(stick / "Sessions")).contains(QStringLiteral("on the stick")));
        std::cout << "ok: the keep controller is told the stick, and refuses a folder on it\n";

        const fs::path keptFolder = scratch / "kept";
        const QVariantMap form{
            {"wavPath", pathToQString(stick / "Sessions" / "K1.wav")},
            {"recording", QStringLiteral("K1.wav")},
            {"artist", QString()},
            {"title", QStringLiteral("K1")},
            {"album", QString()},
            {"genre", QString()},
            {"year", QString()},
            {"folder", pathToQString(keptFolder)},
        };
        assert(keep.keep(form));
        assert(keep.hasWork());
        wired.setAllIncluded(true);
        QSignalSpy finished(&wired, &RecordingsController::finished);
        wired.deleteSelected();
        assert(!wired.working());
        assert(wired.errorMessage().contains(QStringLiteral("being kept")));
        assert(fs::exists(stick / "Sessions" / "K1.wav") && fs::exists(stick / "Sessions" / "K2.wav"));
        std::cout << "ok: nothing is deleted while a recording is being kept, and the page is told why\n";

        release = true;
        QSignalSpy keptSpy(&keep, &KeepRecordingController::finished);
        assert(waitFor(keptSpy, 1));
        assert(!keep.hasWork());
        assert(keptSpy.first().first().toMap().value("state").toString() == QStringLiteral("kept"));

        // While the delete runs, nothing is kept.
        std::atomic<bool> letDelete{false};
        seabass::application::RecordingDeleteHooks hooks;
        hooks.beforeDelete = [&letDelete](const fs::path &) {
            for (int i = 0; i < 5000 && !letDelete.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        RecordingsController::setDeleteHooksForTesting(hooks);
        wired.deleteSelected();
        assert(wired.working());
        QVariantMap second = form;
        second.insert("wavPath", pathToQString(stick / "Sessions" / "K2.wav"));
        second.insert("recording", QStringLiteral("K2.wav"));
        second.insert("title", QStringLiteral("K2"));
        assert(!keep.keep(second));
        assert(keep.errorMessage().contains(QStringLiteral("deleted")));
        assert(!keep.hasWork());
        // Let go of while the delete runs: it still keeps nothing until the
        // delete has ended.
        wired.setKeepController(nullptr);
        assert(!keep.keep(second));
        assert(keep.errorMessage().contains(QStringLiteral("deleted")));
        std::cout << "ok: a keep controller let go of during a delete still keeps nothing meanwhile\n";
        letDelete = true;
        assert(waitFor(finished, 1));
        // Told when it ended: the refusal is another one now.
        QVariantMap noFolder = second;
        noFolder.insert("folder", QString());
        assert(!keep.keep(noFolder));
        assert(!keep.errorMessage().contains(QStringLiteral("deleted")));
        std::cout << "ok: and is told when the delete has ended\n";
        RecordingsController::setDeleteHooksForTesting({});
        assert(!fs::exists(stick / "Sessions" / "K1.wav"));
        assert(fs::exists(keptFolder / "K1.flac"));
        std::cout << "ok: nothing is kept while recordings are being deleted; once the keep has ended, Delete runs\n";
        for (int i = 0; i < 300 && wired.busy(); ++i) {
            wiredListed.wait(100);
        }
        wired.setKeepController(nullptr);
        KeepRecordingController::setPartsForTesting({});
    }

#if defined(SEABASS_HAVE_RECORDING_ENCODER)
    // A recording whose header was not completed: its row says so, with
    // the seconds of audio the file holds, and the seconds the header
    // declares where it declares any (cut short). One without a whole
    // frame holds no audio.
    {
        for (int i = 0; i < 300 && controller.busy(); ++i) {
            listed.wait(100);
        }
        std::string unfinished = wav(44100 * 2, 'u');
        unfinished.replace(40, 4, std::string(4, '\xff'));
        write(stick / "Sessions" / "U1.wav", unfinished);
        std::string cutShort = wav(44100 * 2, 'c');
        const std::uint32_t declared = 44100 * 4 * 4;  // four seconds
        for (int i = 0; i < 4; ++i) {
            cutShort[static_cast<std::size_t>(40 + i)] = static_cast<char>((declared >> (8 * i)) & 0xff);
        }
        cutShort += "\x01\x02";  // a partial last frame
        write(stick / "Sessions" / "C1.wav", cutShort);
        std::string empty = wav(0, 'e');
        empty.replace(40, 4, std::string(4, '\0'));
        write(stick / "Sessions" / "E1.wav", empty);
        write(stick / "Sessions" / "F1.wav", wav(44100, 'f'));
        const int listedBefore = listed.count();
        controller.load(QStringLiteral("STICK"), QString(), pathToQString(engine));
        assert(waitFor(listed, listedBefore + 1));
        auto *model = controller.recordings();
        using Model = seabass::gui::RecordingListModel;
        bool sawUnfinished = false;
        bool sawCutShort = false;
        bool sawEmpty = false;
        bool sawFinished = false;
        for (int i = 0; i < model->rowCount(); ++i) {
            const QModelIndex index = model->index(i);
            const QString name = model->data(index, Model::FileNameRole).toString();
            const bool rowUnfinished = model->data(index, Model::UnfinishedRole).toBool();
            const double held = model->data(index, Model::HeldSecondsRole).toDouble();
            const double declaredSeconds = model->data(index, Model::DeclaredSecondsRole).toDouble();
            const bool noAudio = model->data(index, Model::HoldsNoAudioRole).toBool();
            if (name == QStringLiteral("U1.wav")) {
                sawUnfinished = rowUnfinished && held > 1.99 && held < 2.01 && declaredSeconds < 0 && !noAudio;
            } else if (name == QStringLiteral("C1.wav")) {
                sawCutShort = rowUnfinished && held > 1.99 && held < 2.01 && declaredSeconds > 3.99
                    && declaredSeconds < 4.01 && !noAudio;
            } else if (name == QStringLiteral("E1.wav")) {
                sawEmpty = noAudio && !rowUnfinished;
            } else if (name == QStringLiteral("F1.wav")) {
                sawFinished = !rowUnfinished && held < 0 && declaredSeconds < 0 && !noAudio;
            }
        }
        assert(sawUnfinished);
        assert(sawCutShort);
        std::cout << "ok: a recording cut short is marked with both lengths\n";
        assert(sawEmpty);
        std::cout << "ok: a recording without a whole frame holds no audio\n";
        assert(sawFinished);
        std::cout << "ok: a recording whose header was not completed is marked, with the audio the file holds\n";
    }
#endif

    std::cout << "recordings_controller_test: all passed\n";
    return 0;
}
