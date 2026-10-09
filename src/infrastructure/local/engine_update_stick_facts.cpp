// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/engine_update_stick_facts.hpp"

#include <stdexcept>

#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/local/rekordbox_baseline_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

namespace seabass::infrastructure::local
{

application::EngineUpdateStickFacts readEngineUpdateStickFacts(const std::string &pioneerRoot,
                                                               const std::string &engineLibraryPath)
{
    application::EngineUpdateStickFacts facts;
    facts.stickRoot = paths::stickRootForCatalogPath(engineLibraryPath);
    if (paths::stickRootForCatalogPath(pioneerRoot) != facts.stickRoot) {
        throw std::runtime_error("the rekordbox library (" + pioneerRoot + ") and the Engine library ("
                                 + engineLibraryPath + ") are not on one stick");
    }

    const auto importState = engine::readRekordboxImportState(engineLibraryPath, pioneerRoot);
    if (!importState.error.empty()) {
        throw std::runtime_error(importState.error);
    }
    if (!importState.hasRekordboxLibrary) {
        throw std::runtime_error("could not read export.pdb's sequence under " + pioneerRoot);
    }
    facts.currentSequence = importState.librarySequence;

    facts.rekordboxPlaylists = rekordbox::rekordboxPlaylistTree(pioneerRoot);
    facts.enginePlaylists = engine::listEnginePlaylists(engineLibraryPath);
    facts.enginePdbImportKey = engine::readEnginePdbImportKeys(engineLibraryPath);

    std::string error;
    facts.baseline = readRekordboxBaseline(pathFromUtf8(facts.stickRoot), &error);
    if (!error.empty()) {
        throw std::runtime_error(error);
    }
    facts.fileExists = application::fileExistsUnder(facts.stickRoot);
    return facts;
}

}  // namespace seabass::infrastructure::local
