// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The store of the player's own analyses on the stick
// (infrastructure/local/kept_engine_analysis_file.hpp):
//
// 1. A round trip keeps every value exactly: each SQLite storage class
//    (a NULL, an integer, a real whose bits matter, text, a blob with
//    every byte value), an overview file and the lack of one, a key with a
//    tab, newline and backslash in it.
// 2. The file is where it is said to be, one per key.
// 3. A damaged file is Damaged, never believed: cut short, one byte
//    changed (the checksum), another version, a field missing, a file in
//    another key's place.
// 4. keep() and drop() call the hook first; a hook that throws leaves the
//    file as it was. drop() of nothing is not an error.
// 5. Nothing kept: None.
// 6. Format 1 (no seabassColumnStates, seabassColumnAnalyses or
//    seabassFiles) is read, with those empty; files of 0, 1 and 2 bytes are
//    Damaged, never a crash.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using application::EngineSqlValue;
using application::KeptEngineAnalysis;
using Status = application::KeptEngineAnalysisLookup::Status;
using infrastructure::local::StickKeptEngineAnalysisStore;

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

std::string every256()
{
    std::string bytes;
    for (int i = 0; i < 256; ++i) {
        bytes += static_cast<char>(i);
    }
    return bytes;
}

KeptEngineAnalysis sample(const std::string &key)
{
    KeptEngineAnalysis e;
    e.pathKey = key;
    e.trackFile = "../Contents/Artist\tName/01 Track.flac";
    e.trackId = 42;
    e.libraryUuid = "f5d66faf-8dac-4d2d-bd45-b9bf7ff7f926";
    e.seabassState = std::string(64, 'a');
    e.seabassAnalysis.clear();
    e.seabassColumnStates = {std::string(64, 'b'), std::string(64, 'c')};
    e.seabassColumnAnalyses = {std::string(64, 'd')};
    e.seabassFiles.clear();
    e.original.isAnalyzed = EngineSqlValue::ofInteger(1);
    e.original.isBeatGridLocked = EngineSqlValue::null();
    e.original.bpm = EngineSqlValue::ofInteger(-7);
    // 0.1 is not exact in binary: only its bits round-trip.
    e.original.bpmAnalyzed = EngineSqlValue::ofReal(0.1 + 0.2);
    e.original.key = EngineSqlValue::ofText("8A\n\\");
    e.original.trackData = EngineSqlValue::ofBlob(every256());
    e.original.overviewWaveFormData = EngineSqlValue::ofBlob(std::string("x"));
    e.original.beatData = EngineSqlValue::ofBlob(std::string());
    e.original.overviewFile = every256() + "end";
    e.original.overviewFileRelativePath = "Database2/OverviewData/f5d66faf-8dac-4d2d-bd45-b9bf7ff7f926/42.rgb";
    return e;
}

std::string readAll(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path &file, const std::string &text)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

int main()
{
    const fs::path stick = testing::scratchRoot() / "kept_engine_analysis_file_test";
    fs::remove_all(stick);
    fs::create_directories(stick);
    const std::string key = "contents/artist\tname/01\\track\n.flac";

    // 5. Nothing kept.
    StickKeptEngineAnalysisStore store(stick);
    check(store.find(key).status == Status::None, "nothing kept yet: None");
    store.drop(key);  // nothing to drop: no error
    check(!fs::exists(infrastructure::paths::stickKeptEngineAnalysisDir(stick)), "a drop of nothing makes nothing");

    // 1, 2. Round trip, in its place.
    std::vector<std::string> hooked;
    store.setBeforeChange([&hooked](const std::string &file) { hooked.push_back(file); });
    const KeptEngineAnalysis kept = sample(key);
    store.keep(kept);
    const fs::path file = infrastructure::local::keptEngineAnalysisFile(stick, key);
    check(file.parent_path() == infrastructure::paths::stickKeptEngineAnalysisDir(stick), "under Seabass/engine-analysis");
    check(pathToUtf8(file.filename()).size() == 32 + 4, "named by 32 hex digits");
    check(fs::is_regular_file(file), "the file is there");
    check(hooked.size() == 1 && hooked[0] == pathToUtf8(file), "the hook was told before the write");
    const auto found = store.find(key);
    check(found.status == Status::Found, "found: " + found.error);
    check(found.entry == kept, "every field read back as it was written");
    check(found.entry.original.bpmAnalyzed.real == 0.1 + 0.2, "the real's bits survive");
    check(found.entry.original.beatData.type == EngineSqlValue::Type::Blob && found.entry.original.beatData.bytes.empty(),
          "an empty blob stays a blob");
    check(infrastructure::local::keptEngineAnalysisFile(stick, key + "x") != file, "another key, another file");
    KeptEngineAnalysis noFile = sample(key + "2");
    noFile.original.overviewFile.reset();
    store.keep(noFile);
    check(store.find(key + "2").entry.original.overviewFile == std::nullopt, "no overview file stays none");
    const std::string text = readAll(file);
    check(text.rfind("seabass-engine-analysis\t2\n", 0) == 0, "versioned header");

    // 3. Damaged files are not believed.
    const auto damaged = [&](const std::string &body, const std::string &why) {
        writeAll(file, body);
        const auto r = store.find(key);
        check(r.status == Status::Damaged && !r.error.empty(), why + ": " + r.error);
    };
    damaged(text.substr(0, text.size() - 1), "cut short (no final newline)");
    damaged(text.substr(0, text.size() / 2), "cut in half");
    std::string flipped = text;
    flipped[text.find("trackId\t42") + 8] = '3';
    damaged(flipped, "one byte changed: the checksum");
    std::string version = text;
    version.replace(version.find("\t2\n"), 3, "\t3\n");
    damaged(version, "another version");
    damaged("", "empty");
    // A file whose checksum holds but which belongs to another key.
    writeAll(file, infrastructure::local::serializeKeptEngineAnalysis(sample("someone else")));
    check(store.find(key).status == Status::Damaged, "another key's entry in this key's place");
    // A field missing, the checksum made to match: only the grammar refuses.
    {
        std::string body = text.substr(0, text.find("#sha256"));
        const auto at = body.find("\nkey\t") + 1;
        body.erase(at, body.find('\n', at) - at + 1);
        body += "#sha256\t" + infrastructure::hashing::toHex(infrastructure::hashing::Sha256::of(std::string_view(body)))
            + "\n";
        damaged(body, "a field missing");
    }
    // 6. Tiny files: Damaged, each with its reason.
    damaged("\n", "one byte, a newline");
    writeAll(file, "\n");
    check(store.find(key).error.find("empty but for a newline") != std::string::npos,
          "a lone newline is refused before the checksum is looked for");
    damaged("x", "one byte");
    damaged("\n\n", "two bytes");
    damaged("x\n", "two bytes, one line");
    // Format 1: the same lines but the three lists, read with them empty.
    {
        std::string body = text.substr(0, text.find("#sha256"));
        body.replace(body.find("\t2\n"), 3, "\t1\n");
        for (const std::string name : {"seabassColumnStates", "seabassColumnAnalyses", "seabassFiles"}) {
            const auto at = body.find("\n" + name + "\t") + 1;
            body.erase(at, body.find('\n', at) - at + 1);
        }
        body += "#sha256\t" + infrastructure::hashing::toHex(infrastructure::hashing::Sha256::of(std::string_view(body)))
            + "\n";
        writeAll(file, body);
        const auto v1 = store.find(key);
        KeptEngineAnalysis expected = kept;
        expected.seabassColumnStates.clear();
        expected.seabassColumnAnalyses.clear();
        expected.seabassFiles.clear();
        check(v1.status == Status::Found && v1.entry == expected, "format 1 is read, without the lists: " + v1.error);
    }
    writeAll(file, text);
    check(store.find(key).status == Status::Found, "the good file again");

    // 4. A hook that throws stops the keep and the drop.
    store.setBeforeChange([](const std::string &) { throw std::runtime_error("no backup"); });
    KeptEngineAnalysis other = kept;
    other.trackId = 7;
    bool threw = false;
    try {
        store.keep(other);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    check(threw && store.find(key).entry.trackId == 42, "a refused keep leaves the old entry");
    threw = false;
    try {
        store.drop(key);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    check(threw && fs::exists(file), "a refused drop leaves the file");
    store.setBeforeChange({});
    store.drop(key);
    check(!fs::exists(file) && store.find(key).status == Status::None, "dropped");

    fs::remove_all(stick);
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "kept_engine_analysis_file_test: all checks passed\n";
    return 0;
}
