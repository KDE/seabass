// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The beat grid ledger on the stick (<stick>/Seabass/beat-grids.tsv),
// over scratch directories standing in for sticks:
//
// - a round trip gives the ledger that was written, keys holding a tab,
//   a newline and a backslash among them, each still one line of two
//   fields; the file is the documented text, sorted by key;
// - a missing file is an empty ledger and no error;
// - every way the file can be wrong (another magic word or version, a
//   line with one field or three, a bad escape, an empty or repeated
//   key, keys out of order, a short or uppercase fingerprint, no final
//   newline, an empty file) is an empty ledger with an error naming the
//   file, and nothing of the rest of it is believed;
// - the writer refuses an empty key or a bad fingerprint, leaving the old
//   file as it was, and refuses a stick root that is not a directory.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace seabass::infrastructure::local;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

fs::path freshStick(const std::string &name)
{
    const fs::path dir = testing::scratchRoot() / "beat_grid_ledger_file" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string fingerprint(char digit)
{
    return std::string(64, digit);
}

std::string fileText(const fs::path &stick)
{
    std::ifstream in(infrastructure::paths::stickBeatGridLedger(stick), std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

void putFile(const fs::path &stick, const std::string &text)
{
    const fs::path file = infrastructure::paths::stickBeatGridLedger(stick);
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

int main()
{
    // 1. Round trip.
    {
        const fs::path stick = freshStick("round-trip");
        BeatGridLedger ledger;
        ledger["contents/b/track.mp3"] = fingerprint('b');
        ledger["contents/a/track.mp3"] = fingerprint('a');
        ledger["contents/odd\tname\nwith\\slash.mp3"] = fingerprint('0');
        std::string error;
        check(writeBeatGridLedger(stick, ledger, &error) && error.empty(), "the ledger is written: " + error);
        const BeatGridLedger back = readBeatGridLedger(stick, &error);
        check(error.empty(), "and reads without an error: " + error);
        check(back == ledger, "and reads back as written");
        const std::string want = "seabass-beat-grids\t1\n"
                                 "contents/a/track.mp3\t"
            + fingerprint('a') + "\ncontents/b/track.mp3\t" + fingerprint('b')
            + "\ncontents/odd\\tname\\nwith\\\\slash.mp3\t" + fingerprint('0') + "\n";
        check(fileText(stick) == want, "the file is the documented text, sorted by key, escaped");
        check(serializeBeatGridLedger(ledger) == want, "and is what serializeBeatGridLedger() says");

        check(writeBeatGridLedger(stick, {}, &error), "an empty ledger is written");
        check(readBeatGridLedger(stick, &error).empty() && error.empty(), "and reads back empty, no error");
        std::cout << "case 1 (round trip) done\n";
    }

    // 2. No file.
    {
        const fs::path stick = freshStick("missing");
        std::string error = "stale";
        check(readBeatGridLedger(stick, &error).empty() && error.empty(), "a missing file is an empty ledger, no error");
        std::cout << "case 2 (missing file) done\n";
    }

    // 3. Malformed files: empty, with an error, nothing of them believed.
    {
        const std::string good = "contents/a.mp3\t" + fingerprint('a') + "\n";
        const std::vector<std::pair<std::string, std::string>> bad = {
            {"empty", ""},
            {"other-magic", "seabass-beat-grid\t1\n" + good},
            {"other-version", "seabass-beat-grids\t2\n" + good},
            {"no-final-newline", "seabass-beat-grids\t1\n" + good + "contents/b.mp3\t" + fingerprint('b')},
            {"one-field", "seabass-beat-grids\t1\n" + good + "contents/b.mp3\n"},
            {"three-fields", "seabass-beat-grids\t1\n" + good + "contents/b.mp3\t" + fingerprint('b') + "\tx\n"},
            {"bad-escape", "seabass-beat-grids\t1\n" + good + "contents/b\\x.mp3\t" + fingerprint('b') + "\n"},
            {"empty-key", "seabass-beat-grids\t1\n\t" + fingerprint('b') + "\n" + good},
            {"repeated-key", "seabass-beat-grids\t1\n" + good + good},
            {"out-of-order", "seabass-beat-grids\t1\ncontents/z.mp3\t" + fingerprint('b') + "\n" + good},
            {"short-fingerprint", "seabass-beat-grids\t1\n" + good + "contents/b.mp3\t" + std::string(63, 'b') + "\n"},
            {"uppercase-fingerprint", "seabass-beat-grids\t1\n" + good + "contents/b.mp3\t" + std::string(64, 'B') + "\n"},
            {"blank-line", "seabass-beat-grids\t1\n" + good + "\n"},
        };
        for (const auto &[name, text] : bad) {
            const fs::path stick = freshStick("bad-" + name);
            putFile(stick, text);
            std::string error;
            const BeatGridLedger read = readBeatGridLedger(stick, &error);
            check(read.empty(), name + ": an empty ledger");
            check(!error.empty() && error.find("beat-grids.tsv") != std::string::npos,
                  name + ": an error naming the file, got \"" + error + "\"");
        }
        std::cout << "case 3 (malformed files) done\n";
    }

    // 4. The writer's refusals.
    {
        const fs::path stick = freshStick("refusals");
        std::string error;
        BeatGridLedger ledger{{"contents/a.mp3", fingerprint('a')}};
        check(writeBeatGridLedger(stick, ledger, &error), "a good ledger is written");
        const std::string before = fileText(stick);
        check(!writeBeatGridLedger(stick, {{"", fingerprint('a')}}, &error) && !error.empty(), "an empty key is refused");
        check(!writeBeatGridLedger(stick, {{"contents/b.mp3", "abc"}}, &error) && !error.empty(),
              "a short fingerprint is refused");
        check(!writeBeatGridLedger(stick, {{"contents/b.mp3", std::string(64, 'A')}}, &error) && !error.empty(),
              "an uppercase one too");
        check(fileText(stick) == before, "the old file is as it was");
        check(!writeBeatGridLedger(stick / "not-there", ledger, &error) && !error.empty(),
              "a stick root that is not a directory is refused");
        check(!fs::exists(stick / "not-there"), "and not made");
        std::cout << "case 4 (the writer's refusals) done\n";
    }

    if (failures == 0) {
        std::cout << "beat_grid_ledger_file_test: all cases passed\n";
    }
    return failures == 0 ? 0 : 1;
}
