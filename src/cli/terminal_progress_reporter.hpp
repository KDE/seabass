// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/progress_reporter.hpp"

namespace seabass::cli
{

// Renders an in-place terminal progress bar (or, when the total is
// unknown, a running count). Silently does nothing when stdout isn't a
// terminal, so redirected/piped output stays clean.
class TerminalProgressReporter : public application::ProgressReporter
{
public:
    void start(const std::string &label, size_t total) override;
    void tick(size_t current) override;
    void finish() override;
    void warn(const std::string &message) override;

private:
    void render();
    void clearLine();

    std::string m_label;
    size_t m_total = 0;
    size_t m_current = 0;
    size_t m_suppressedWarnings = 0;
    bool m_active = false;
};

}  // namespace seabass::cli
