// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Engine's write session plans for every change of the save that writes
// m.db (sharedFormatWriteSession, SaveContext::changesWriting): a save of
// Add Cue changes, which each ask for no scratch copy, goes through one
// when there are enough of them. On a copy of the anonymized fixture:
// 1. twenty Add Cue changes on Engine: the save works on a scratch copy
//    of m.db, and every cue is in the stick's own m.db afterwards;
// 2. one Add Cue change: no scratch copy, and the cue is in m.db too.
// What says whether a copy was used is a change of the test's own at the
// head of the save, which asks the write session as the first change
// writing Engine (hint 0, as a cue change asks).

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "domain/track.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

// Asks the save's Engine write session first, and says whether it put
// m.db behind a scratch copy.
class WriteSessionProbe : public PendingChange
{
public:
    explicit WriteSessionProbe(QString engine)
        : m_engine(std::move(engine))
    {
    }
    QString id() const override { return QStringLiteral("test:write-session-probe"); }
    QString description() const override { return QStringLiteral("asks the write session"); }
    QString unit() const override { return QStringLiteral("probes"); }
    QStringList formatsTouched() const override { return {QStringLiteral("engine")}; }
    ChangeOutcome apply(SaveContext &ctx) override
    {
        usedScratch = sharedFormatWriteSession(ctx, "engine", m_engine.toStdString(), 0, "probe").usesScratch();
        asked = true;
        return ChangeOutcome::skip();
    }
    bool asked = false;
    bool usedScratch = false;

private:
    QString m_engine;
};

constexpr int Pad = 8;
constexpr double PositionMs = 30000.0;

std::shared_ptr<AddCueChange> addCue(const testing::EngineChangeStick &stick, const std::string &id)
{
    return std::make_shared<AddCueChange>(QStringLiteral("engine"), stick.enginePath(), QString::fromStdString(id),
                                          PositionMs, QStringLiteral("hot"), Pad, QStringLiteral("#FF0000"),
                                          QStringLiteral("hint"), false, 0.0, QStringLiteral("a track"));
}

// Whether the stick's own m.db, read afresh, holds the cue on track `id`.
bool cueLanded(const testing::EngineChangeStick &stick, const std::string &id)
{
    infrastructure::engine::LibdjinteropEngineReader reader(stick.engineUtf8());
    const auto track = reader.readTrack(id);
    if (!track) {
        return false;
    }
    for (const domain::CuePoint &cue : track->cues) {
        // Within an MP3's lead-in of where it was asked for: the file is
        // not on the fixture's stick, and the writer and the reader may
        // place it by different lead-ins.
        if (cue.kind == domain::CuePoint::Kind::Hot && cue.hotCueNumber == Pad && std::abs(cue.positionMs - PositionMs) < 100.0) {
            return true;
        }
    }
    return false;
}

void run(const fs::path &fixture, const std::string &name, int cues, bool scratchExpected)
{
    const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, name);
    const auto rows = testing::sqlRows(stick.db, "SELECT id FROM Track ORDER BY id LIMIT " + std::to_string(cues) + ";");
    assert(static_cast<int>(rows.size()) == cues);
    auto probe = std::make_shared<WriteSessionProbe>(stick.enginePath());
    std::vector<std::shared_ptr<PendingChange>> changes{probe};
    for (const auto &row : rows) {
        assert(!cueLanded(stick, row[0]) && "the fixture's track has no such cue yet");
        changes.push_back(addCue(stick, row[0]));
    }
    const SaveLoopResult saved = testing::saveChanges(changes, QString(), stick.enginePath());
    if (!saved.error.isEmpty()) {
        std::cerr << name << ": " << saved.error.toStdString() << "\n";
    }
    assert(saved.error.isEmpty() && saved.appliedIds.size() == changes.size());
    assert(probe->asked);
    std::cout << name << ": " << cues << " Add Cue change(s), " << (probe->usedScratch ? "a" : "no")
              << " scratch copy of m.db\n";
    assert(probe->usedScratch == scratchExpected);
    for (const auto &row : rows) {
        assert(cueLanded(stick, row[0]) && "the cue is in the stick's own m.db");
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_write_session_hint_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "engine_write_session_hint_home");

    // 1. Twenty: more than the fixture's 3.4 MB m.db is worth writing
    //    item by item (infrastructure::shouldUseWholeFileReplace), though
    //    the first change to ask wants no copy.
    run(fixture, "engine_hint_twenty_cues", 20, true);
    std::cout << "case 1 (twenty Add Cue changes go through a scratch copy and land in m.db) OK\n";

    // 2. One: straight into m.db.
    run(fixture, "engine_hint_one_cue", 1, false);
    std::cout << "case 2 (one Add Cue change writes m.db itself, through the save's write session) OK\n";

    std::cout << "engine_write_session_hint_test: all cases passed\n";
    return 0;
}
