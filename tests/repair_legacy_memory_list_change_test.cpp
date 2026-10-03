// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// RepairLegacyMemoryListChange through the real save loop (#55): a
// track's .DAT with the stale memory list of 5282555e and a hung
// player's ANLZ0001.DAT beside it comes out with the list in shape, its
// entry kept, the debris gone, and both files backed up for Undo.

#include <QString>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/repair_legacy_memory_list_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/legacy_memory_list_audit.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::gui;
using namespace seabass::infrastructure::rekordbox;
namespace fs = std::filesystem;

namespace
{

std::string fromHex(const std::string &hex)
{
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// after-seabass/P025/0000297B/ANLZ0000.DAT of the #33 evidence: one entry
// at 149 ms under a header that says "empty".
const std::string StaleHeaderOneEntry = fromHex(
    "50434f4200000018000000500000000000000001ffffffff504350540000001c00000038000000000000000000010000ffffffff"
    "010003e800000095ffffffff00000000000000000000000000000000");

std::string memoryListOf(const fs::path &dat)
{
    for (const auto &section : AnlzFile::readRaw(pathToUtf8(dat)).sections) {
        if (section.fourcc == 0x50434f42 && readU32BE(section.rawBytes, 12) == 0) {
            return section.rawBytes;
        }
    }
    return {};
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: repair_legacy_memory_list_change_test <anonymized_library dir>\n";
        return 2;
    }
    const fs::path fixtureDat = pathFromUtf8(argv[1]) / "rekordbox" / "USBANLZ" / "P017" / "0002435D" / "ANLZ0000.DAT";
    assert(fs::exists(fixtureDat));

    const fs::path stick = testing::scratchRoot() / "seabass_repair_legacy_memory_list_change_test";
    fs::remove_all(stick);
    const fs::path pioneer = stick / "PIONEER";
    const fs::path dir = pioneer / "USBANLZ" / "P001" / "00000001";
    fs::create_directories(dir);
    {
        AnlzFile file = AnlzFile::readRaw(pathToUtf8(fixtureDat));
        for (auto &section : file.sections) {
            if (section.fourcc == 0x50434f42 && readU32BE(section.rawBytes, 12) == 0) {
                section.rawBytes = StaleHeaderOneEntry;
            }
        }
        file.writeRaw(pathToUtf8(dir / "ANLZ0000.DAT"));
    }
    fs::copy_file(fixtureDat, dir / "ANLZ0000.EXT");
    {
        // The RX2's skeleton: empty beat grid, waveforms and cue lists.
        AnlzFile skeleton = AnlzFile::readRaw(pathToUtf8(fixtureDat));
        for (auto &section : skeleton.sections) {
            switch (section.fourcc) {
            case 0x5051545A:
                section.rawBytes = fromHex("5051545a0000001800000018000000000008000000000000");
                break;
            case 0x50574156:
                section.rawBytes = fromHex("5057415600000014000000140000000000010000");
                break;
            case 0x50575632:
                section.rawBytes = fromHex("5057563200000014000000140000000000010000");
                break;
            default:
                break;
            }
        }
        skeleton.writeRaw(pathToUtf8(dir / "ANLZ0001.DAT"));
    }

    const std::string analyzePath = "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT";
    const auto named = [&analyzePath](const std::string &p) { return p == analyzePath; };
    auto finding = auditTrackAnalysis(pathToUtf8(pioneer), analyzePath, named);
    assert(finding && finding->shape.headerStale && finding->debris.size() == 1);

    domain::Track track;
    track.format = "rekordbox";
    track.sourceId = "7";
    track.title = "Codec";
    track.artist = "Aender";

    // Through the save loop, as Library Health stages it.
    application::CancellationToken token;
    SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, pathToQString(pioneer), {});
    std::vector<std::shared_ptr<PendingChange>> changes;
    changes.push_back(std::make_shared<RepairLegacyMemoryListChange>(track, *finding));
    assert(changes[0]->id() == RepairLegacyMemoryListChange::idFor("7"));
    assert(changes[0]->filesToBackup(ctx).size() == 2 && "the .DAT and the debris are both backed up");
    const SaveLoopResult result = runSaveLoop(changes, ctx);
    assert(result.error.isEmpty() && !result.cancelled);
    assert(result.appliedIds.size() == 1);

    // The list is in shape with its entry, the debris is gone, the
    // sibling is not, and the track audits clean.
    const std::string after = memoryListOf(dir / "ANLZ0000.DAT");
    AnlzLegacyCueCodec::checkSection(after);
    assert(readU32BE(after, 20) == 0 && "the header names the last entry");
    assert(after.substr(24) == StaleHeaderOneEntry.substr(24) && "the entry is kept byte for byte");
    assert(!fs::exists(dir / "ANLZ0001.DAT") && "the debris is gone");
    assert(fs::exists(dir / "ANLZ0000.EXT") && "the .EXT is not");
    assert(!auditTrackAnalysis(pathToUtf8(pioneer), analyzePath, named).has_value());
    std::cout << "case 1 (stale list rebuilt, entry kept, debris removed) OK\n";

    fs::remove_all(stick);
    std::cout << "all cases passed\n";
    return 0;
}
