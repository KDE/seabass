// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

namespace seabass::infrastructure::process
{

struct CommandResult
{
    int exitCode = -1;
    std::string output;  // combined stdout+stderr
};

// Runs a command directly by argv (no shell involved, so arguments --
// device paths, D-Bus GVariant literals, PowerShell script paths -- never
// need shell escaping), capturing its combined stdout+stderr. args[0] is
// the program to run (searched via PATH). Two platform implementations:
// run_command_posix.cpp (fork/exec, extracted from UdisksctlMediaMounter,
// its original caller, once LinuxUsbFormatter needed the exact same
// primitive for "gdbus call") and run_command_windows.cpp (CreateProcess
// with a pipe, args re-quoted into the single command-line string
// CreateProcess expects).
CommandResult runCommand(const std::vector<std::string> &args);

}  // namespace seabass::infrastructure::process
