// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/app_data_directory.hpp"

#include "infrastructure/paths/seabass_paths.hpp"

namespace seabass::infrastructure::local
{

std::filesystem::path appDataDirectory()
{
    return paths::localMetadataDir();
}

}  // namespace seabass::infrastructure::local
