// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace seabass::infrastructure::engine
{

// The domain-level "did the restore work": opens the restored Engine
// database with the same reader the rest of the app uses and lists every
// local (non-streaming) track whose file is not there. nullopt when the
// target has no Engine database. Plugs into
// application::RestoreOptions::libraryCheck.
std::optional<std::vector<std::string>> checkRestoredEngineLibrary(const std::filesystem::path &targetRoot);

}  // namespace seabass::infrastructure::engine
