// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// dj_software_independence_test <command> [args...]
//
// Runs <command> while a process called "rekordbox" is alive, and passes
// only if the command does. The command is a non-live test that saves,
// so this is the suite saying it does not depend on what else runs on
// the machine: three suite runs on 2026-11-19 went red with "Refused:
// rekordbox appears to be running" because the release rig had its fake
// rekordbox up at the time, and a developer with the real one open would
// see the same.
//
// The fake is first seen through the real process scan, from here. A
// fake the guard would not have recognised would make the pass mean
// nothing.
//
// Linux only: prctl() is how a forked child takes the name, as in
// rekordbox_process_detector_test.

#include "infrastructure/system/rekordbox_process_detector.hpp"

#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: dj_software_independence_test <command> [args...]\n";
        return 2;
    }

    const pid_t fake = ::fork();
    if (fake < 0) {
        std::cerr << "fork failed\n";
        return 1;
    }
    if (fake == 0) {
        ::prctl(PR_SET_NAME, "rekordbox", 0, 0, 0);
        for (;;) {
            ::pause();
        }
    }
    const auto stopFake = [fake] {
        ::kill(fake, SIGKILL);
        int status = 0;
        ::waitpid(fake, &status, 0);
    };

    bool seen = false;
    for (int attempt = 0; attempt < 100 && !seen; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        seen = seabass::infrastructure::system::isRekordboxRunning();
    }
    if (!seen) {
        stopFake();
        std::cerr << "FAIL: the fake rekordbox (pid " << fake << ") was not seen by the real scan, "
                  << "so a pass would prove nothing\n";
        return 1;
    }
    std::cout << "fake rekordbox running as pid " << fake << ", seen by the real scan\n" << std::flush;

    const pid_t test = ::fork();
    if (test < 0) {
        stopFake();
        std::cerr << "fork failed\n";
        return 1;
    }
    if (test == 0) {
        ::execvp(argv[1], argv + 1);
        std::cerr << "cannot run " << argv[1] << "\n";
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(test, &status, 0);

    // Still there at the end, or the command may have run without it.
    const bool stillSeen = seabass::infrastructure::system::isRekordboxRunning();
    stopFake();
    if (!stillSeen) {
        std::cerr << "FAIL: the fake rekordbox was gone before the command finished\n";
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "FAIL: the command failed while a process called rekordbox was running"
                  << (WIFEXITED(status) ? " (exit " + std::to_string(WEXITSTATUS(status)) + ")" : std::string())
                  << "\n";
        return 1;
    }
    std::cout << "the command passed with rekordbox running\n";
    return 0;
}
