// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/ports/progress_reporter.hpp"

#include <iostream>

namespace seabass::application
{

NullProgressReporter &NullProgressReporter::instance()
{
    static NullProgressReporter reporter;
    return reporter;
}

void NullProgressReporter::warn(const std::string &message)
{
    std::cerr << "warning: " << message << "\n";
}

}  // namespace seabass::application
