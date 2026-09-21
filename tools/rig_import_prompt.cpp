// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: whether a Seabass save leaves a Denon player asking to
// re-import the rekordbox library -- the rig half of issue #42.
//
//   rig_import_prompt <stick root> --record <file>
//   rig_import_prompt <stick root> --compare <file> [--identical]
//
// Engine keeps the rekordbox library's sequence number, as it stood when
// Engine last imported it, in Information.lastRekordBoxLibraryImportRead-
// Counter; export.pdb carries that sequence in its header and moves it
// every time the library is written. Level, and the player says nothing
// on insert. Apart, and it offers to rebuild the Engine side from the
// rekordbox one -- which is what overwrites cover art with
// "image://fileart//<path on the importing computer>" and undoes every
// Engine-side repair this app has made. Measured on a Prime 4 and a
// Prime Go+, 2026-09-18.
//
// The gap this check is for: any Seabass save that rewrites export.pdb
// moves that sequence, so a repair today means a prompt tomorrow
// offering to overwrite the very library the repair just fixed.
//
// --record writes the pair down. --compare reads them back and answers
// the question that matters:
//
//   - level before and apart after: FAIL. Seabass armed the prompt.
//   - apart before and apart after: PASS, said out loud. The library had
//     already moved on before this round touched it, and #42 is explicit
//     that quietly swallowing that case would hide something real.
//   - level before and level after: PASS.
//
// --identical is the stricter form, for a check that saved nothing at
// all: neither number may have moved, in either direction. A "leave and
// discard" that still shifts the sequence has written to the stick.
//
// Reads only. The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL",
// and the exit code matches.

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "infrastructure/engine/engine_import_state.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

struct Recorded
{
    bool hasEngine = false;
    bool hasRekordbox = false;
    std::uint64_t engineCounter = 0;
    std::uint64_t librarySequence = 0;
    bool asking = false;
};

infrastructure::engine::RekordboxImportState readState(const fs::path &root)
{
    return infrastructure::engine::readRekordboxImportState((root / "Engine Library").string(),
                                                            (root / "PIONEER").string());
}

void describe(const infrastructure::engine::RekordboxImportState &state)
{
    std::cout << "engine library: " << (state.hasEngineLibrary ? "yes" : "no")
              << ", rekordbox library: " << (state.hasRekordboxLibrary ? "yes" : "no") << "\n";
    std::cout << "engine last imported: " << state.engineCounter << ", library is at " << state.librarySequence << "\n";
    if (!state.error.empty()) {
        std::cout << "error: " << state.error << "\n";
    }
    std::cout << "the player would " << (state.playerWillOfferImport() ? "ASK" : "say nothing") << " on insert\n";
}

bool write(const fs::path &file, const infrastructure::engine::RekordboxImportState &state)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << (state.hasEngineLibrary ? 1 : 0) << "\n"
        << (state.hasRekordboxLibrary ? 1 : 0) << "\n"
        << state.engineCounter << "\n"
        << state.librarySequence << "\n"
        << (state.playerWillOfferImport() ? 1 : 0) << "\n";
    return static_cast<bool>(out);
}

bool read(const fs::path &file, Recorded &out)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    int hasEngine = 0;
    int hasRekordbox = 0;
    int asking = 0;
    in >> hasEngine >> hasRekordbox >> out.engineCounter >> out.librarySequence >> asking;
    if (!in) {
        return false;
    }
    out.hasEngine = hasEngine != 0;
    out.hasRekordbox = hasRekordbox != 0;
    out.asking = asking != 0;
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 4) {
        std::cerr << "usage: rig_import_prompt <stick root> --record <file>\n"
                  << "       rig_import_prompt <stick root> --compare <file> [--identical]\n";
        return 2;
    }
    const fs::path root = argv[1];
    const std::string mode = argv[2];
    const fs::path file = argv[3];
    bool identical = false;
    for (int i = 4; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--identical") {
            identical = true;
        } else {
            std::cerr << "unknown argument: " << flag << "\n";
            return 2;
        }
    }
    if (mode != "--record" && mode != "--compare") {
        std::cerr << "unknown mode: " << mode << "\n";
        return 2;
    }
    if (identical && mode != "--compare") {
        std::cerr << "--identical only means anything with --compare\n";
        return 2;
    }

    try {
        const infrastructure::engine::RekordboxImportState now = readState(root);
        describe(now);

        if (mode == "--record") {
            if (!write(file, now)) {
                std::cout << "could not write " << file << "\nRIG RESULT: FAIL\n";
                return 1;
            }
            std::cout << "recorded to " << file << "\nRIG RESULT: PASS\n";
            return 0;
        }

        Recorded before;
        if (!read(file, before)) {
            std::cout << "could not read " << file << ", so there is nothing to compare against\nRIG RESULT: FAIL\n";
            return 1;
        }
        std::cout << "before: engine " << before.engineCounter << ", library " << before.librarySequence << ", player would "
                  << (before.asking ? "ASK" : "say nothing") << "\n";

        // A stick that lost one of its two libraries between the two
        // reads is not a comparison anyone can draw a conclusion from.
        if (before.hasEngine != now.hasEngineLibrary || before.hasRekordbox != now.hasRekordboxLibrary) {
            std::cout << "the stick does not carry the same pair of libraries it did before\nRIG RESULT: FAIL\n";
            return 1;
        }
        if (!now.error.empty()) {
            std::cout << "could not read the state now\nRIG RESULT: FAIL\n";
            return 1;
        }
        if (!now.hasEngineLibrary || !now.hasRekordboxLibrary) {
            std::cout << "only one of the two libraries is here, so no player would ever compare them\n"
                      << "RIG RESULT: PASS\n";
            return 0;
        }

        if (identical) {
            const bool same = before.engineCounter == now.engineCounter && before.librarySequence == now.librarySequence;
            if (!same) {
                std::cout << "nothing was saved, but the numbers moved: engine " << before.engineCounter << " -> "
                          << now.engineCounter << ", library " << before.librarySequence << " -> " << now.librarySequence
                          << "\nRIG RESULT: FAIL\n";
                return 1;
            }
            std::cout << "unchanged, as nothing was saved\nRIG RESULT: PASS\n";
            return 0;
        }

        if (!before.asking && now.playerWillOfferImport()) {
            std::cout << "the save armed the import prompt: the library moved to " << now.librarySequence
                      << " and Engine is still at " << now.engineCounter << ", so the next insert offers to overwrite "
                      << "the Engine side with the rekordbox one (issue #42)\nRIG RESULT: FAIL\n";
            return 1;
        }
        if (before.asking) {
            std::cout << "the player was already going to ask before this round touched the stick, and still is; "
                      << "that is the stick's own history, not something this save did\nRIG RESULT: PASS\n";
            return 0;
        }
        std::cout << "still level, so the player stays quiet\nRIG RESULT: PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cout << "threw: " << error.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }
}
