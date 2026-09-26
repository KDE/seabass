// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// BackupAdvisorController::forget() against steps in every state, through
// the fingerprint reader seam: a fake that blocks until the test releases
// it, or until the step's token is cancelled, so "while the step runs" is
// a state the test holds rather than a race it hopes to hit.
//
// - A stick forgotten while its first step is queued is never read, and
//   never appears in the advice.
// - A stick forgotten while its first step runs does not come back when
//   that step lands.
// - A stick forgotten while its second (Cues) step waits on the cue pass
//   has that step cancelled: the wait ends at once rather than after the
//   pass.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "domain/library_fingerprint.hpp"
#include "domain/track.hpp"
#include "gui/backup_advisor_controller.hpp"
#include "gui/library_catalog_cache.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::application::CancellationToken;
using seabass::domain::LibraryFingerprint;
using seabass::gui::BackupAdvisorController;
using seabass::gui::FingerprintPass;

namespace
{

LibraryFingerprint someLibrary(bool cuesKnown)
{
    std::vector<seabass::domain::Track> tracks;
    for (int i = 0; i < 20; ++i) {
        seabass::domain::Track track;
        track.title = "Title " + std::to_string(i);
        track.artist = "Artist";
        track.durationSeconds = 200 + i;
        tracks.push_back(track);
    }
    return seabass::domain::fingerprintLibrary(tracks, cuesKnown);
}

// The fake reader. Each (stick, pass) either answers at once or, when
// held, blocks until released or until its token is cancelled. Counts its
// calls and records how each held call ended.
struct FakeReader
{
    std::mutex mutex;
    std::condition_variable cv;
    std::map<QString, int> calls;              // by "rekordboxPath|pass"
    std::map<QString, bool> held;              // same key: block when true
    std::map<QString, bool> entered;           // a held call is waiting
    std::map<QString, bool> endedByCancel;     // how a held call ended
    std::map<QString, std::chrono::milliseconds> cancelLatency;

    static QString key(const QString &path, FingerprintPass pass)
    {
        return path + (pass == FingerprintPass::Tracks ? QStringLiteral("|tracks") : QStringLiteral("|cues"));
    }

    std::optional<LibraryFingerprint> read(const QString &rekordboxPath, FingerprintPass pass, CancellationToken cancel)
    {
        const QString k = key(rekordboxPath, pass);
        std::unique_lock<std::mutex> lock(mutex);
        ++calls[k];
        if (held[k]) {
            entered[k] = true;
            cv.notify_all();
            // A deadline so a missing cancel fails the test rather than
            // hanging it.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (held[k] && !cancel.cancelled() && std::chrono::steady_clock::now() < deadline) {
                cv.wait_for(lock, std::chrono::milliseconds(5));
            }
            endedByCancel[k] = cancel.cancelled();
        }
        if (cancel.cancelled()) {
            return std::nullopt;
        }
        // A rekordbox stick: the first step does not know its cues yet.
        return someLibrary(pass == FingerprintPass::Cues);
    }

    void waitEntered(const QString &k)
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool ok = cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered[k]; });
        assert(ok && "the held step started");
        (void)ok;
    }

    void release(const QString &k)
    {
        std::lock_guard<std::mutex> lock(mutex);
        held[k] = false;
        cv.notify_all();
    }

    bool hasEntered(const QString &k)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return entered[k];
    }

    int callCount(const QString &k)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return calls[k];
    }
};

void waitUntilIdle(BackupAdvisorController &controller)
{
    QElapsedTimer timer;
    timer.start();
    while (controller.busy()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        assert(timer.elapsed() < 20000 && "the advisor drains its queue");
    }
}

struct Stick
{
    QString mountPoint;
    QString pioneer;
};

Stick makeStick(const fs::path &root, const char *name)
{
    const fs::path mount = root / name;
    fs::create_directories(mount);
    // Paths that name no catalog: the cache's prefetch, started by the
    // first step, fails in the background and caches nothing.
    return {QString::fromStdString(seabass::pathToUtf8(mount)),
            QString::fromStdString(seabass::pathToUtf8(mount / "PIONEER"))};
}

void wire(BackupAdvisorController &controller, FakeReader &fake)
{
    controller.setFingerprintReaderForTesting(
        [&fake](const QString &rekordboxPath, const QString &, FingerprintPass pass, CancellationToken cancel) {
            return fake.read(rekordboxPath, pass, std::move(cancel));
        });
}

void forgetWhileFactsQueuedOrRunning(const fs::path &root)
{
    FakeReader fake;
    BackupAdvisorController controller;
    wire(controller, fake);
    const Stick a = makeStick(root, "queued_a");
    const Stick b = makeStick(root, "queued_b");
    const Stick c = makeStick(root, "queued_c");
    const QString aTracks = FakeReader::key(a.pioneer, FingerprintPass::Tracks);
    const QString bTracks = FakeReader::key(b.pioneer, FingerprintPass::Tracks);
    fake.held[aTracks] = true;

    controller.assess(QStringLiteral("A"), a.mountPoint, a.pioneer, QString());
    controller.assess(QStringLiteral("B"), b.mountPoint, b.pioneer, QString());
    controller.assess(QStringLiteral("C"), c.mountPoint, c.pioneer, QString());
    fake.waitEntered(aTracks);
    assert(controller.pending().contains(b.mountPoint) && "B's first step is queued behind A's");

    // B while its first step is queued, A while its first step runs.
    controller.forget(b.mountPoint);
    assert(!controller.pending().contains(b.mountPoint) && "B's queued step is gone");
    controller.forget(a.mountPoint);
    fake.release(aTracks);
    waitUntilIdle(controller);

    assert(fake.callCount(bTracks) == 0 && "a stick forgotten while queued is never read");
    const QVariantMap advice = controller.advice();
    assert(!advice.contains(b.mountPoint) && "and never appears in the advice");
    assert(!advice.contains(a.mountPoint) && "a stick forgotten while its step ran does not come back");
    assert(advice.contains(c.mountPoint) && "the stick nobody forgot is advised, and A is not among its peers");
    assert(!controller.busy() && controller.pending().isEmpty());
    std::cout << "forget() while the first step is queued or running: the stick never comes back OK\n";
}

void forgetCancelsTheCuesStep(const fs::path &root)
{
    FakeReader fake;
    BackupAdvisorController controller;
    wire(controller, fake);
    const Stick a = makeStick(root, "cues_a");
    const QString aCues = FakeReader::key(a.pioneer, FingerprintPass::Cues);
    fake.held[aCues] = true;

    controller.assess(QStringLiteral("A"), a.mountPoint, a.pioneer, QString());
    // The first step answers with cues unknown, so a Cues step follows
    // and waits (the fake holds it, as the cue pass would).
    QElapsedTimer timer;
    timer.start();
    while (!fake.hasEntered(aCues)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        assert(timer.elapsed() < 10000 && "the Cues step started");
    }
    assert(controller.advice().contains(a.mountPoint) && controller.busy());

    timer.restart();
    controller.forget(a.mountPoint);
    waitUntilIdle(controller);
    const qint64 elapsed = timer.elapsed();
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        assert(fake.endedByCancel[aCues] && "forget() cancels the Cues step it is running");
    }
    assert(elapsed < 5000 && "and the advisor is free again at once, not after the pass");
    assert(!controller.advice().contains(a.mountPoint));
    std::cout << "forget() cancels a running Cues step (" << elapsed << " ms to idle) OK\n";
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const fs::path root = seabass::testing::scratchRoot() / "backup_advisor_controller_test";
    fs::remove_all(root);
    fs::create_directories(root);

    forgetWhileFactsQueuedOrRunning(root);
    forgetCancelsTheCuesStep(root);

    seabass::gui::LibraryCatalogCache::instance().waitUntilPrefetchIdle();
    fs::remove_all(root);
    std::cout << "All backup_advisor_controller tests passed.\n";
    return 0;
}
