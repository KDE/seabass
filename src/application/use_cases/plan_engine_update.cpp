// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/plan_engine_update.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

#include "application/path_key.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::application
{

std::string stickRelativePathOf(const std::string &filePath, const std::string &stickRoot)
{
    if (filePath.empty() || stickRoot.empty()) {
        return {};
    }
    const std::filesystem::path relative =
        pathFromUtf8(filePath).lexically_normal().lexically_relative(pathFromUtf8(stickRoot).lexically_normal());
    const std::string text = pathToGenericUtf8(relative);
    // lexically_relative climbs out with ".." for a path elsewhere, and
    // answers "" for one on another root name (another drive letter).
    if (text.empty() || text == "." || text.rfind("..", 0) == 0) {
        return {};
    }
    return text;
}

std::function<bool(const std::string &)> fileExistsUnder(const std::string &stickRoot)
{
    return [root = pathFromUtf8(stickRoot)](const std::string &stickRelativePath) {
        if (stickRelativePath.empty()) {
            return false;
        }
        std::error_code ec;
        return std::filesystem::is_regular_file(root / pathFromUtf8(stickRelativePath), ec);
    };
}

domain::EngineUpdateInput buildEngineUpdateInput(std::vector<domain::Track> rekordbox,
                                                 std::vector<domain::Track> engine, EngineUpdateStickFacts facts)
{
    domain::EngineUpdateInput in;
    in.rekordbox = std::move(rekordbox);
    in.engine = std::move(engine);
    in.rekordboxPlaylists = std::move(facts.rekordboxPlaylists);
    in.enginePlaylists = std::move(facts.enginePlaylists);
    in.baseline = std::move(facts.baseline);
    in.currentSequence = facts.currentSequence;
    in.enginePdbImportKey = std::move(facts.enginePdbImportKey);
    in.stickRelativeOf = [root = std::move(facts.stickRoot)](const std::string &filePath) {
        return stickRelativePathOf(filePath, root);
    };
    in.pathKeyOf = [](const std::string &stickRelativePath) { return normalizedPathKey(stickRelativePath); };
    in.fileExists = std::move(facts.fileExists);
    return in;
}

}  // namespace seabass::application
