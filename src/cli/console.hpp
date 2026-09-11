// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::cli
{

enum class Color { Default, Red, Green, Yellow, Cyan, Gray, Bold };

// Structured, colored terminal output for the CLI. Colors are disabled
// automatically when stdout isn't a terminal or when NO_COLOR is set.
// Default operation is informational (info()); verbose() only prints when
// verbose mode has been enabled.
class Console
{
public:
    static void setVerbose(bool verbose);
    static bool isVerbose();

    static void info(const std::string &message);
    static void verbose(const std::string &message);
    static void warn(const std::string &message);
    static void error(const std::string &message);
    static void heading(const std::string &message);

    // Prompts on stdout and reads a y/n answer from stdin. Returns false
    // (declining) without prompting when stdin isn't a terminal, so a
    // non-interactive run never blocks waiting for input it'll never get.
    static bool confirm(const std::string &question);
    static bool isInteractive();

    static std::string colorize(const std::string &text, Color color);

private:
    static bool s_verbose;
};

}  // namespace seabass::cli
