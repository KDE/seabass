// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The analysis ledger on the stick (<stick>/Seabass/analysis.tsv.gz,
// src/infrastructure/local/analysis_ledger_file.hpp), over scratch
// directories standing in for sticks:
//
// - a round trip gives the ledger that was written, field for field, a
//   key, a version and a key name holding a tab, a newline and a
//   backslash among them; the file is gzip around the documented text,
//   sorted by key, with a #sha256 line over everything above it;
// - a missing file is an empty ledger and no error;
// - every way the file can be wrong (not gzip, gzip cut short, bytes
//   after the gzip, inflating past 64 MiB, no #sha256 line, a hash that
//   does not match, another magic word or version, an unknown line kind,
//   a track line with a field too few or too many, a field that does not
//   parse, keys out of order or repeated, no final newline) is an empty
//   ledger, damaged, with an error naming the file and that reason; a
//   folder in its place cannot be read, which is not damage;
// - an entry that cannot be written is left out of what is written, the
//   others kept;
// - the writer refuses what it cannot write as it is, leaves the old file
//   as it was and calls no hook, and calls the hook with the file's path
//   before it writes;
// - tempos are written with exactly two decimals.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/local/analysis_ledger_file.hpp"
#include "infrastructure/local/gzip_text.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace seabass::infrastructure::local;
namespace hashing = seabass::infrastructure::hashing;

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

const auto NoHook = [](const std::string &) {};

fs::path freshStick(const std::string &name)
{
    const fs::path dir = testing::scratchRoot() / "analysis_ledger_file" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string readBytes(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void putBytes(const fs::path &stick, const std::string &bytes)
{
    const fs::path file = infrastructure::paths::stickAnalysisLedger(stick);
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string gzip(const std::string &text)
{
    std::string problem;
    const auto bytes = gzipBytes(text, &problem);
    if (!bytes) {
        std::cerr << "gzip: " << problem << "\n";
        std::exit(2);
    }
    return *bytes;
}

std::string gunzipped(const std::string &bytes)
{
    std::istringstream in(bytes);
    std::string out;
    std::string problem;
    if (!gunzip(in, false, 1u << 26, &out, &problem)) {
        return "<not gzip: " + problem + ">";
    }
    return out;
}

std::string sealed(const std::string &body)
{
    return body + "#sha256\t" + hashing::toHex(hashing::Sha256::of(body)) + "\n";
}

AnalysisLedgerEntry entry(const std::string &key, char grid)
{
    AnalysisLedgerEntry e;
    e.pathKey = key;
    e.fileSize = 1234567;
    e.fileMtimeUnix = 1764160000;
    e.inputRevision = 1;
    e.gridRevision = 2;
    e.keyRevision = 1;
    e.waveformRevision = 1;
    e.libraryVersion = "0.3.0";
    e.tempoMin = "70.00";
    e.tempoMax = "180.00";
    e.rekordboxGrid = std::string(64, grid);
    e.engineGrid = std::string(64, 'e');
    e.key = "F#m";
    e.keyWritten = true;
    e.analysedAtUnix = 1764163600;
    return e;
}

const std::string Header = "seabass-analysis\t1\n";
const std::string GoodLine = "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\n";

}  // namespace

int main()
{
    // 1. Round trip, and the file is the documented text.
    {
        const fs::path stick = freshStick("round-trip");
        AnalysisLedger ledger;
        ledger["contents/b/track.mp3"] = entry("contents/b/track.mp3", 'b');
        ledger["contents/a/track.mp3"] = entry("contents/a/track.mp3", 'a');
        AnalysisLedgerEntry odd = entry("contents/odd\tname\nwith\\slash.mp3", '0');
        odd.rekordboxGrid.clear();
        odd.engineGrid.clear();
        odd.key = "A\tm\\";
        odd.keyWritten = false;
        odd.libraryVersion = "0.3.0\tlocal";
        odd.fileMtimeUnix = -5;
        ledger[odd.pathKey] = odd;
        AnalysisLedgerEntry keyless = entry("contents/c.wav", 'c');
        keyless.key.clear();
        keyless.keyWritten = false;
        ledger[keyless.pathKey] = keyless;

        std::vector<std::string> hooked;
        std::string error;
        check(writeAnalysisLedger(stick, ledger, [&](const std::string &file) { hooked.push_back(file); }, &error)
                  && error.empty(),
              "the ledger is written: " + error);
        check(hooked.size() == 1
                  && hooked[0] == pathToUtf8(infrastructure::paths::stickAnalysisLedger(stick)),
              "the hook is called once, with the file's path, before the write");
        const AnalysisLedger back = readAnalysisLedger(stick, &error);
        check(error.empty(), "and reads without an error: " + error);
        check(back == ledger, "and reads back as written, field for field");

        const std::string body =
            Header
            + "t\tcontents/a/track.mp3\t1234567\t1764160000\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t" + std::string(64, 'a')
            + "\t" + std::string(64, 'e') + "\tF#m\t1\t1764163600\n"
            + "t\tcontents/b/track.mp3\t1234567\t1764160000\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t" + std::string(64, 'b')
            + "\t" + std::string(64, 'e') + "\tF#m\t1\t1764163600\n"
            + "t\tcontents/c.wav\t1234567\t1764160000\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t" + std::string(64, 'c') + "\t"
            + std::string(64, 'e') + "\t-\t0\t1764163600\n"
            + "t\tcontents/odd\\tname\\nwith\\\\slash.mp3\t1234567\t-5\t1\t2\t1\t1\t0.3.0\\tlocal\t70.00\t180.00\t-\t-\t"
              "A\\tm\\\\\t0\t1764163600\n";
        const std::string want = sealed(body);
        const std::string bytes = readBytes(infrastructure::paths::stickAnalysisLedger(stick));
        check(bytes.size() > 2 && static_cast<unsigned char>(bytes[0]) == 0x1f
                  && static_cast<unsigned char>(bytes[1]) == 0x8b,
              "the file is gzip");
        check(gunzipped(bytes) == want, "the file is the documented text, sorted by key, escaped, sealed");
        std::string problem;
        check(serializeAnalysisLedger(ledger, &problem) == want, "and is what serializeAnalysisLedger() says");

        check(writeAnalysisLedger(stick, {}, NoHook, &error), "an empty ledger is written");
        check(gunzipped(readBytes(infrastructure::paths::stickAnalysisLedger(stick))) == sealed(Header),
              "as its header and its #sha256 line");
        check(readAnalysisLedger(stick, &error).empty() && error.empty(), "and reads back empty, no error");
        std::cout << "case 1 (round trip, the documented text) done\n";
    }

    // 2. No file.
    {
        const fs::path stick = freshStick("missing");
        std::string error = "stale";
        check(readAnalysisLedger(stick, &error).empty() && error.empty(), "a missing file is an empty ledger, no error");
        std::cout << "case 2 (missing file) done\n";
    }

    // 3. Damaged files: empty, with an error naming the file.
    {
        const std::string good = sealed(Header + GoodLine);
        const std::string goodGzip = gzip(good);
        {
            const fs::path stick = freshStick("good-control");
            putBytes(stick, goodGzip);
            std::string error;
            check(readAnalysisLedger(stick, &error).size() == 1 && error.empty(),
                  "the control file every damaged one is made from reads: " + error);
        }
        const auto line = [](const std::string &key) {
            return "t\t" + key + "\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\n";
        };
        const std::vector<std::tuple<std::string, std::string, std::string>> bad = {
            {"empty", "", "it is empty"},
            {"not-gzip", good, "not intact gzip data"},
            {"gzip-cut-short", goodGzip.substr(0, goodGzip.size() - 9), "it is truncated"},
            {"bytes-after-gzip", goodGzip + "x", "bytes after the end of the compressed data"},
            {"no-sha256", gzip(Header + GoodLine), "has no #sha256 line"},
            {"hash-mismatch", gzip(Header + GoodLine + "#sha256\t" + std::string(64, '0') + "\n"), "do not match its #sha256 line"},
            {"hash-uppercase",
             gzip(Header + GoodLine + "#sha256\t" + [&] {
                 std::string hex = hashing::toHex(hashing::Sha256::of(Header + GoodLine));
                 for (char &c : hex) {
                     c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                 }
                 return hex;
             }() + "\n"), "#sha256 line is malformed"},
            {"no-final-newline", gzip(good.substr(0, good.size() - 1)), "does not end with a newline"},
            {"other-magic", gzip(sealed("seabass-analyses\t1\n" + GoodLine)), "not a Seabass analysis ledger"},
            {"other-version", gzip(sealed("seabass-analysis\t2\n" + GoodLine)), "format version 2"},
            {"header-extra-field", gzip(sealed("seabass-analysis\t1\tx\n" + GoodLine)), "its header has 3 fields"},
            {"unknown-kind", gzip(sealed(Header + GoodLine + "x\tcontents/b.wav\n")), "unknown line kind \"x\""},
            {"field-too-few", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\n")), "a track line of 15 fields"},
            {"field-too-many", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\t9\n")), "a track line of 17 fields"},
            {"empty-key", gzip(sealed(Header + line(""))), "no key, or a key with a bad escape"},
            {"bad-escape", gzip(sealed(Header + line("contents/b\\x.wav"))), "no key, or a key with a bad escape"},
            {"negative-size", gzip(sealed(Header + "t\tcontents/a.wav\t-10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\n")), "size or time is not a decimal number"},
            {"revision-not-a-number", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\tx\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\n")), "a revision is not a decimal number"},
            {"negative-revision", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t-1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\n")), "a revision is not a decimal number"},
            {"tempo-one-decimal", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.0\t180.00\t-\t-\t-\t0\t30\n")), "tempo range is not two numbers"},
            {"tempo-exponent", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t7e1.00\t180.00\t-\t-\t-\t0\t30\n")), "tempo range is not two numbers"},
            {"short-fingerprint", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t" + std::string(63, 'a') + "\t-\t-\t0\t30\n")), "grid fingerprint is neither"},
            {"uppercase-fingerprint", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t" + std::string(64, 'A') + "\t-\t0\t30\n")), "grid fingerprint is neither"},
            {"empty-key-name", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t\t0\t30\n")), "the key is empty or has a bad escape"},
            {"key-written-without-key", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t1\t30\n")), "keyWritten is not 0 or 1"},
            {"key-written-not-a-bool", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\tAm\t2\t30\n")), "keyWritten is not 0 or 1"},
            {"time-not-a-number", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\tsoon\n")), "time of the analysis is not a decimal number"},
            {"repeated-key", gzip(sealed(Header + line("contents/a.wav") + line("contents/a.wav"))), "a key listed twice"},
            {"out-of-order", gzip(sealed(Header + line("contents/z.wav") + line("contents/a.wav"))), "keys out of order"},
            {"blank-line", gzip(sealed(Header + GoodLine + "\n")), "unknown line kind \"\""},
            {"no-header", gzip(sealed(GoodLine)), "not a Seabass analysis ledger"},
        };
        for (const auto &[name, bytes, reason] : bad) {
            const fs::path stick = freshStick("bad-" + name);
            putBytes(stick, bytes);
            std::string error;
            AnalysisLedgerFault fault = AnalysisLedgerFault::None;
            const AnalysisLedger read = readAnalysisLedger(stick, &error, &fault);
            check(read.empty(), name + ": an empty ledger");
            check(!error.empty() && error.find("analysis.tsv.gz") != std::string::npos,
                  name + ": an error naming the file, got \"" + error + "\"");
            check(error.find(reason) != std::string::npos,
                  name + ": refused for \"" + reason + "\", got \"" + error + "\"");
            check(fault == AnalysisLedgerFault::Damaged, name + ": damaged, so a save replaces it");
        }
        // More than the cap once inflated: a small file, refused before it
        // takes the memory.
        {
            const fs::path stick = freshStick("bad-inflates-too-far");
            const std::string bomb = gzip(Header + std::string(64u * 1024u * 1024u, '\n'));
            check(bomb.size() < 1024u * 1024u, "the file inflating past the cap is small");
            putBytes(stick, bomb);
            std::string error;
            AnalysisLedgerFault fault = AnalysisLedgerFault::None;
            check(readAnalysisLedger(stick, &error, &fault).empty()
                      && error.find("inflates to more than 67108864 bytes") != std::string::npos
                      && fault == AnalysisLedgerFault::Damaged,
                  "a file inflating past 64 MiB is refused for that, got \"" + error + "\"");
        }
        // A folder where the file belongs cannot be read: no ledger, and
        // not one a save may replace.
        {
            const fs::path stick = freshStick("bad-folder");
            fs::create_directories(infrastructure::paths::stickAnalysisLedger(stick));
            std::string error;
            AnalysisLedgerFault fault = AnalysisLedgerFault::None;
            check(readAnalysisLedger(stick, &error, &fault).empty() && error.find("analysis.tsv.gz") != std::string::npos
                      && error.find("not a regular file") != std::string::npos,
                  "a folder in the file's place: an empty ledger and an error, got \"" + error + "\"");
            check(fault == AnalysisLedgerFault::Unreadable, "a folder in the file's place cannot be read, not damaged");
        }
        // A good file, a missing one: no fault.
        {
            const fs::path stick = freshStick("good-fault");
            std::string error;
            AnalysisLedgerFault fault = AnalysisLedgerFault::Damaged;
            readAnalysisLedger(stick, &error, &fault);
            check(fault == AnalysisLedgerFault::None, "a missing file is no fault");
            putBytes(stick, goodGzip);
            fault = AnalysisLedgerFault::Damaged;
            check(readAnalysisLedger(stick, &error, &fault).size() == 1 && fault == AnalysisLedgerFault::None,
                  "a good file is no fault");
        }
        std::cout << "case 3 (damaged files refused, each for its reason, " << bad.size() + 2 << " ways) done\n";
    }

    // 4. The writer refuses what it cannot write as it is, the old file
    //    untouched and the hook not called.
    {
        const fs::path stick = freshStick("refusals");
        AnalysisLedger ledger;
        ledger["contents/a.wav"] = entry("contents/a.wav", 'a');
        std::string error;
        check(writeAnalysisLedger(stick, ledger, NoHook, &error), "the first ledger is written: " + error);
        const std::string before = readBytes(infrastructure::paths::stickAnalysisLedger(stick));

        std::vector<std::pair<std::string, AnalysisLedger>> refused;
        {
            AnalysisLedger l;
            l[""] = entry("", 'a');
            refused.emplace_back("empty key", l);
        }
        {
            AnalysisLedger l;
            l["contents/b.wav"] = entry("contents/a.wav", 'a');
            refused.emplace_back("a key that is not its entry's", l);
        }
        {
            AnalysisLedger l;
            auto e = entry("contents/b.wav", 'a');
            e.tempoMin = "70";
            l[e.pathKey] = e;
            refused.emplace_back("a tempo without its decimals", l);
        }
        {
            AnalysisLedger l;
            auto e = entry("contents/b.wav", 'a');
            e.engineGrid = "-";
            l[e.pathKey] = e;
            refused.emplace_back("a fingerprint of \"-\" instead of empty", l);
        }
        {
            AnalysisLedger l;
            auto e = entry("contents/b.wav", 'a');
            e.gridRevision = -1;
            l[e.pathKey] = e;
            refused.emplace_back("a negative revision", l);
        }
        {
            AnalysisLedger l;
            auto e = entry("contents/b.wav", 'a');
            e.key.clear();
            e.keyWritten = true;
            l[e.pathKey] = e;
            refused.emplace_back("a key written and none named", l);
        }
        {
            AnalysisLedger l;
            auto e = entry("contents/b.wav", 'a');
            e.key = "-";
            l[e.pathKey] = e;
            refused.emplace_back("a key of \"-\"", l);
        }
        for (const auto &[what, l] : refused) {
            bool hooked = false;
            const bool written = writeAnalysisLedger(stick, l, [&](const std::string &) { hooked = true; }, &error);
            check(!written && error.find("analysis.tsv.gz") != std::string::npos,
                  what + ": refused with an error naming the file, got \"" + error + "\"");
            check(!hooked, what + ": the hook is not called");
            check(readBytes(infrastructure::paths::stickAnalysisLedger(stick)) == before, what + ": the old file stays");
        }
        const fs::path notADirectory = stick / "a-file";
        std::ofstream(notADirectory) << "x";
        bool hooked = false;
        check(!writeAnalysisLedger(notADirectory, ledger, [&](const std::string &) { hooked = true; }, &error)
                  && !hooked,
              "a stick root that is not a directory is refused before the hook");
        check(!writeAnalysisLedger(stick / "gone", ledger, NoHook, &error) && !fs::exists(stick / "gone"),
              "a stick root that is not there is refused and not made");
        std::cout << "case 4 (the writer's refusals) done\n";
    }

    // 4b. One entry that cannot be written keeps no other off the stick.
    {
        AnalysisLedger ledger;
        ledger["contents/a.wav"] = entry("contents/a.wav", 'a');
        auto bad = entry("contents/b.wav", 'b');
        bad.tempoMin = "70";
        ledger[bad.pathKey] = bad;
        ledger["contents/c.wav"] = entry("contents/c.wav", 'c');
        std::vector<std::string> dropped;
        const AnalysisLedger kept = writableAnalysisLedger(ledger, &dropped);
        check(kept.size() == 2 && kept.count("contents/a.wav") == 1 && kept.count("contents/c.wav") == 1,
              "the writable entries are kept");
        check(dropped.size() == 1 && dropped[0].find("contents/b.wav") != std::string::npos
                  && dropped[0] == analysisLedgerEntryProblem("contents/b.wav", bad),
              "the one left out is named with its problem");
        std::string problem;
        check(serializeAnalysisLedger(kept, &problem).has_value(), "and the rest serialises");
        std::cout << "case 4b (an entry that cannot be written is left out, the rest kept) done\n";
    }

    // 5. Tempos: two decimals, nothing else.
    {
        check(formatTempoBpm(70.0) == "70.00", "70 is 70.00, got " + formatTempoBpm(70.0));
        check(formatTempoBpm(175.0) == "175.00", "175 is 175.00");
        check(formatTempoBpm(87.5) == "87.50", "87.5 is 87.50");
        check(formatTempoBpm(88.004) == "88.00", "88.004 is 88.00");
        check(formatTempoBpm(88.006) == "88.01", "88.006 is 88.01");
        check(formatTempoBpm(0.0) == "0.00", "0 is 0.00");
        check(formatTempoBpm(-1.0).empty() && formatTempoBpm(1e9).empty()
                  && formatTempoBpm(std::numeric_limits<double>::quiet_NaN()).empty(),
              "a tempo out of range or not a number has no text");
        std::cout << "case 5 (tempo text) done\n";
    }

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "analysis_ledger_file_test: all passed\n";
    return 0;
}
