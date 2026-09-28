// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The catalog cache's prefetch worker at the end of the process
// (docs/async-requests.md, "The end of the process"). Found by the storm:
// the app quit while the backup advisor's prefetch was reading a stick's
// Full stage, endProcess() saw no read running -- the prefetch thread was
// not one it counted -- and returned, and the static destructors ran under
// the pass: its progress reporter was gone and the pass died on a pure
// virtual call ("terminate called without an active exception").
//
// The child stands in for main(): a cache with passes queued, a static
// whose destructor aborts if a pass is still running then (the reporter,
// the readers' statics), and the ending main() has. The parent wants a
// clean exit, and the passes still queued never started.

#include <QCoreApplication>
#include <QProcess>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "gui/async_request.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/process_end.hpp"

using seabass::gui::LibraryCatalogCache;

namespace
{

std::atomic<int> passesRunning{0};
// Which catalogs a pass was started for: each prefetch reads its catalog's
// three stages, one after the other.
std::mutex startedMutex;
std::set<std::string> catalogsStarted;

// Destroyed with the other statics, after main() returns.
struct StandsInForTheReporter
{
    ~StandsInForTheReporter()
    {
        if (passesRunning.load() > 0) {
            std::cerr << "a static was destroyed under a prefetch pass\n";
            std::abort();
        }
    }
} standsInForTheReporter;

int child(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    auto stage = [](LibraryCatalogCache::Detail, const std::string &, const std::string &path,
                    std::vector<seabass::domain::Track> &, LibraryCatalogCache::StageNotes &,
                    seabass::application::ProgressReporter &, seabass::application::CancellationToken cancel) {
        {
            std::lock_guard<std::mutex> lock(startedMutex);
            catalogsStarted.insert(path);
        }
        ++passesRunning;
        // A pass that notices nothing: a slow stick between two files.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        --passesRunning;
        (void)cancel;
    };
    auto mtime = [](const std::string &, const std::string &) { return std::chrono::system_clock::time_point{}; };
    // Never freed, like a cache a let-go worker may still be in.
    auto *cache = new LibraryCatalogCache(stage, mtime);
    cache->prefetch("rekordbox", "/storm/a");
    cache->prefetch("rekordbox", "/storm/b");
    cache->prefetch("rekordbox", "/storm/c");
    while (passesRunning.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const int result = seabass::gui::endProcess(0, std::chrono::milliseconds(5000));
    std::cout << "catalogs read: " << catalogsStarted.size() << std::endl;
    return result;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && std::string(argv[1]) == "child") {
        return child(argc, argv);
    }
    QCoreApplication app(argc, argv);
    QProcess process;
    process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("child")});
    if (!process.waitForFinished(30000)) {
        std::cerr << "FAIL: the child did not end with a prefetch running\n";
        process.kill();
        return 1;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        std::cerr << "FAIL: the child ended badly (status " << process.exitStatus() << ", code "
                  << process.exitCode() << "): " << process.readAllStandardError().toStdString() << "\n";
        return 1;
    }
    const QString out = QString::fromUtf8(process.readAllStandardOutput());
    if (!out.contains(QStringLiteral("catalogs read: 1"))) {
        std::cerr << "FAIL: a catalog was read after the process began to end: " << out.toStdString() << "\n";
        return 1;
    }
    std::cout << "prefetch_at_exit_test: the end of the process waits for a prefetch pass and starts no other\n";
    return 0;
}
