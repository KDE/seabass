// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The end of the process with a read still stuck (docs/async-requests.md,
// "The end of the process"). A worker let go of on a pulled stick may be
// inside LibraryCatalogCache or the readers under it when main() returns,
// and static destructors must not run beneath it. The child below stands
// in for main(): a static whose destructor aborts if a worker is still
// running plays the part of the cache, a request is left stuck, and the
// child ends the way main() does. The parent runs it and wants a clean
// exit: a return that ran static destructors under the stuck worker is
// an abort.

#include <QCoreApplication>
#include <QObject>
#include <QProcess>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>

#include "gui/async_request.hpp"

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
    return seabass::gui::exitAfterAsyncWork(0, std::chrono::milliseconds(200));
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
    std::cout << "async_request_exit_test: a stuck worker outlives no static\n";
    return 0;
}
