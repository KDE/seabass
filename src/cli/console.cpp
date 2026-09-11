// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "cli/console.hpp"

#include <cstdlib>
#include <iostream>
#include <unistd.h>

namespace seabass::cli
{

bool Console::s_verbose = false;

namespace
{

bool colorEnabled()
{
    static const bool enabled = (::isatty(STDOUT_FILENO) != 0) && (std::getenv("NO_COLOR") == nullptr);
    return enabled;
}

const char *ansiCode(Color color)
{
    switch (color) {
    case Color::Red:
        return "\033[31m";
    case Color::Green:
        return "\033[32m";
    case Color::Yellow:
        return "\033[33m";
    case Color::Cyan:
        return "\033[36m";
    case Color::Gray:
        return "\033[90m";
    case Color::Bold:
        return "\033[1m";
    case Color::Default:
        break;
    }
    return "";
}

}  // namespace

void Console::setVerbose(bool verbose)
{
    s_verbose = verbose;
}

bool Console::isVerbose()
{
    return s_verbose;
}

std::string Console::colorize(const std::string &text, Color color)
{
    if (!colorEnabled() || color == Color::Default) {
        return text;
    }
    return std::string(ansiCode(color)) + text + "\033[0m";
}

void Console::info(const std::string &message)
{
    std::cout << message << "\n";
}

void Console::verbose(const std::string &message)
{
    if (s_verbose) {
        std::cout << colorize(message, Color::Gray) << "\n";
    }
}

void Console::warn(const std::string &message)
{
    std::cerr << colorize("warning: ", Color::Yellow) << message << "\n";
}

void Console::error(const std::string &message)
{
    std::cerr << colorize("error: ", Color::Red) << message << "\n";
}

void Console::heading(const std::string &message)
{
    std::cout << colorize(message, Color::Bold) << "\n";
}

bool Console::isInteractive()
{
    static const bool result = ::isatty(STDIN_FILENO) != 0;
    return result;
}

bool Console::confirm(const std::string &question)
{
    if (!isInteractive()) {
        return false;
    }
    std::cout << question << " [y/N] " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
        return false;
    }
    return line == "y" || line == "Y" || line == "yes";
}

}  // namespace seabass::cli
