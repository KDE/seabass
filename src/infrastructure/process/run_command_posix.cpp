// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/process/run_command.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <array>

namespace seabass::infrastructure::process
{

// argv is built *before* forking: some callers (e.g. UdisksctlMediaMounter,
// running alongside this project's own udev monitor thread) run background
// threads, so between fork() and exec() the child must not touch the heap
// (or any other lock another thread might have held at fork time) -- only
// async-signal-safe calls are safe there. Building argv in the child used
// to risk exactly that kind of deadlock.
CommandResult runCommand(const std::vector<std::string> &args)
{
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);

    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        return {-1, "failed to create pipe"};
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return {-1, "fork failed"};
    }

    if (pid == 0) {
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        close(pipeFds[0]);
        close(pipeFds[1]);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipeFds[1]);
    std::string output;
    std::array<char, 4096> buffer;
    ssize_t n;
    while ((n = read(pipeFds[0], buffer.data(), buffer.size())) > 0) {
        output.append(buffer.data(), static_cast<size_t>(n));
    }
    close(pipeFds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {exitCode, output};
}

}  // namespace seabass::infrastructure::process
