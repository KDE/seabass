// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The end of the process with reads still stuck and a write just done
// (docs/async-requests.md, "The end of the process"). A worker let go of on a pulled stick may be
// inside LibraryCatalogCache or the readers under it when main() returns,
// and static destructors must not run beneath it. The child below stands
// in for main(): a static whose destructor aborts if a worker is still
// running plays the part of the cache, a request is left stuck, and the
// child ends the way main() does. The parent runs it and wants a clean
// exit: a return that ran static destructors under the stuck worker is
// an abort. It also wants the child to exit at all with a read stuck the
// way a folder listing or a Browse scan gets stuck (runRead()), which on
// the writes' pool kept the process alive for good; and to have run what
// a write handed on by its page leaves behind -- the lock given back --
// before it exits, which with no event loop left it did not.

#include <QCoreApplication>
#include <QObject>
#include <QProcess>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>

#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <thread>

#include "gui/async_request.hpp"
#include "gui/detached_completion.hpp"
#include "gui/process_end.hpp"

using seabass::application::CancellationToken;
using seabass::gui::AsyncRequest;
using seabass::gui::AsyncWorkers;

namespace
{

// Destroyed with the other statics, after main() returns.
struct StandsInForTheCatalogCache
{
    ~StandsInForTheCatalogCache()
    {
        if (AsyncWorkers::instance().live() > 0) {
            std::cerr << "a static was destroyed under a running worker\n";
            std::abort();
        }
    }
} standsInForTheCatalogCache;

int child(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Never opened: the worker stays stuck for the rest of the process.
    auto gate = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
    {
        QObject owner;
        AsyncRequest<int> request(&owner, nullptr);
        request.start(QStringLiteral("stuck"), QString(), [gate](CancellationToken) {
            std::unique_lock<std::mutex> lock(gate->first);
            gate->second.wait(lock, [] { return false; });
            return 0;
        }, {});
        AsyncWorkers::instance().beginShutdown();
    }
    // A read that is not a page's request, stuck the same way.
    auto stuckRead = seabass::gui::runRead([gate]() {
        std::unique_lock<std::mutex> lock(gate->first);
        gate->second.wait(lock, [] { return false; });
        return 0;
    });
    (void)stuckRead;
    // A write on the writes' pool, handed on by a page that went: done a
    // moment after the pages are gone, its lock given back after that.
    auto write = QtConcurrent::run([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 1;
    });
    seabass::gui::whenWriteEnds(write, []() {
        std::cout << "lock given back" << std::endl;
    });
    return seabass::gui::endProcess(0, std::chrono::milliseconds(200));
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
        std::cerr << "FAIL: the child did not end with a worker stuck\n";
        process.kill();
        return 1;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        std::cerr << "FAIL: the child ended badly (status " << process.exitStatus() << ", code "
                  << process.exitCode() << "): " << process.readAllStandardError().toStdString() << "\n";
        return 1;
    }
    const QString out = QString::fromUtf8(process.readAllStandardOutput());
    if (!out.contains(QStringLiteral("lock given back"))) {
        std::cerr << "FAIL: a write handed on by its page ended without giving its lock back\n";
        return 1;
    }
    std::cout << "async_request_exit_test: a stuck read keeps nothing alive, a finished write gives its lock back\n";
    return 0;
}
