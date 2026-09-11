// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "cli/terminal_progress_reporter.hpp"

#include <algorithm>
#include <iostream>
#include <unistd.h>

#include "cli/console.hpp"

namespace seabass::cli
{

namespace
{

bool isTerminal()
{
    static const bool result = ::isatty(STDOUT_FILENO) != 0;
    return result;
}

constexpr int BarWidth = 30;

}  // namespace

void TerminalProgressReporter::start(const std::string &label, size_t total)
{
    m_label = label;
    m_total = total;
    m_current = 0;
    m_suppressedWarnings = 0;
    m_active = isTerminal();
    if (m_active) {
        render();
    }
}

void TerminalProgressReporter::tick(size_t current)
{
    m_current = current;
    if (m_active) {
        render();
    }
}

void TerminalProgressReporter::finish()
{
    clearLine();
    m_active = false;

    if (m_suppressedWarnings > 0) {
        Console::warn(std::to_string(m_suppressedWarnings) +
                       " warning(s) hidden -- re-run with --verbose to see them");
    }
}

void TerminalProgressReporter::warn(const std::string &message)
{
    // Per-item warnings are detail, not baseline output: only show them in
    // --verbose, but keep a count so finish() can still surface that
    // something was skipped.
    if (!Console::isVerbose()) {
        m_suppressedWarnings++;
        return;
    }

    // Clear the in-progress bar's line before printing, so the warning
    // doesn't land in the middle of it, then redraw the bar afterwards.
    clearLine();
    Console::warn(message);
    if (m_active) {
        render();
    }
}

void TerminalProgressReporter::clearLine()
{
    if (m_active) {
        std::cout << "\r" << std::string(m_label.size() + BarWidth + 20, ' ') << "\r" << std::flush;
    }
}

void TerminalProgressReporter::render()
{
    std::cout << "\r" << m_label << " ";
    if (m_total > 0) {
        double fraction = std::min(1.0, static_cast<double>(m_current) / static_cast<double>(m_total));
        int filled = static_cast<int>(fraction * BarWidth);
        std::cout << "[" << std::string(filled, '#') << std::string(BarWidth - filled, '-') << "] " << m_current
                   << "/" << m_total;
    } else {
        std::cout << m_current;
    }
    std::cout << std::flush;
}

}  // namespace seabass::cli
