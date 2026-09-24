// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/process/run_command.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <string>
#include <vector>

namespace seabass::infrastructure::process
{

namespace
{

// The command line is assembled in UTF-8 (every argument is, by the rule
// in utf8_path.hpp: script paths and device paths come through here) and
// widened once for CreateProcessW. The A variant would read the bytes in
// the ANSI code page and hand the child a mangled path.
std::wstring widen(const std::string &utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

// The standard argv[i]->CreateProcess command-line quoting algorithm
// (matches what CommandLineToArgvW/the Windows CRT's own argv parser
// expects on the way back in) -- every argument gets wrapped in quotes
// and internal quotes/backslash runs before a quote are escaped. Needed
// here because CreateProcess takes one command-line string, not an argv
// array the way execvp does, so this project has to do the quoting
// fork/exec never needed on the POSIX side.
std::string quoteArg(const std::string &arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string quoted = "\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == '\\') {
            ++backslashes;
            ++it;
        }
        if (it == arg.end()) {
            quoted.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted.push_back('"');
        } else {
            quoted.append(backslashes, '\\');
            quoted.push_back(*it);
        }
    }
    quoted.push_back('"');
    return quoted;
}

std::string buildCommandLine(const std::vector<std::string> &args)
{
    std::string commandLine;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            commandLine.push_back(' ');
        }
        commandLine += quoteArg(args[i]);
    }
    return commandLine;
}

}  // namespace

CommandResult runCommand(const std::vector<std::string> &args)
{
    if (args.empty()) {
        return {-1, "no command given"};
    }

    SECURITY_ATTRIBUTES saAttr = {};
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!::CreatePipe(&readPipe, &writePipe, &saAttr, 0)) {
        return {-1, "failed to create pipe"};
    }
    // The parent's read end must never be inherited by the child, or the
    // child holding it open would keep the pipe from ever signalling EOF
    // once the child itself exits.
    ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(STARTUPINFOW);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdOutput = writePipe;
    startupInfo.hStdError = writePipe;
    startupInfo.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION processInfo = {};
    const std::wstring commandLine = widen(buildCommandLine(args));
    // CreateProcessW's lpCommandLine must be a mutable buffer -- it can
    // rewrite embedded nulls/whitespace in place.
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    BOOL started = ::CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                     nullptr, nullptr, &startupInfo, &processInfo);
    ::CloseHandle(writePipe);
    if (!started) {
        ::CloseHandle(readPipe);
        return {-1, "failed to start " + args[0]};
    }

    std::string output;
    std::array<char, 4096> buffer;
    DWORD bytesRead = 0;
    while (::ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) &&
           bytesRead > 0) {
        output.append(buffer.data(), bytesRead);
    }
    ::CloseHandle(readPipe);

    ::WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = 1;
    ::GetExitCodeProcess(processInfo.hProcess, &exitCode);
    ::CloseHandle(processInfo.hProcess);
    ::CloseHandle(processInfo.hThread);

    return {static_cast<int>(exitCode), output};
}

}  // namespace seabass::infrastructure::process
