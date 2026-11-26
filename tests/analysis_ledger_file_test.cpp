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
// - tempos are written with exactly two decimals, a grid's confidence
//   with three and a key's margin with two, "-" where there is none;
// - a read that stops short of the file's size cannot be read, which is
//   not damage; a file larger than a ledger compresses to is damaged
//   without being read;
// - a ledger that can be read and not written (a folder where the
//   temporary file goes) stays as it was, the write saying it failed;
// - a ledger that cannot be used is moved aside, never deleted.

#include <cctype>
#include <chrono>
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
    e.gridConfidence = "0.875";
    e.gridFollowsTempo = "0";
    e.keyMargin = "4.20";
    return e;
}

const std::string Header = "seabass-analysis\t1\n";
const std::string GoodLine = "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\t-\t-\t-\n";
// GoodLine up to its time, for a line whose last three fields a case sets.
const std::string LineToTime = "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30";

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
        // No grid of Seabass's: neither grid field; and no margin.
        odd.gridConfidence.clear();
        odd.gridFollowsTempo.clear();
        odd.keyMargin.clear();
        ledger[odd.pathKey] = odd;
        AnalysisLedgerEntry keyless = entry("contents/c.wav", 'c');
        keyless.key.clear();
        keyless.keyWritten = false;
        keyless.keyMargin.clear();
        keyless.gridConfidence = "1.000";
        keyless.gridFollowsTempo = "1";
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
            + "\t" + std::string(64, 'e') + "\tF#m\t1\t1764163600\t0.875\t0\t4.20\n"
            + "t\tcontents/b/track.mp3\t1234567\t1764160000\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t" + std::string(64, 'b')
            + "\t" + std::string(64, 'e') + "\tF#m\t1\t1764163600\t0.875\t0\t4.20\n"
            + "t\tcontents/c.wav\t1234567\t1764160000\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t" + std::string(64, 'c') + "\t"
            + std::string(64, 'e') + "\t-\t0\t1764163600\t1.000\t1\t-\n"
            + "t\tcontents/odd\\tname\\nwith\\\\slash.mp3\t1234567\t-5\t1\t2\t1\t1\t0.3.0\\tlocal\t70.00\t180.00\t-\t-\t"
              "A\\tm\\\\\t0\t1764163600\t-\t-\t-\n";
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
            return "t\t" + key + "\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\t-\t-\t-\n";
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
            {"field-too-few", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\t-\t-\n")), "a track line of 18 fields"},
            {"field-too-many", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\t-\t-\t-\t9\n")), "a track line of 20 fields"},
            {"empty-key", gzip(sealed(Header + line(""))), "no key, or a key with a bad escape"},
            {"bad-escape", gzip(sealed(Header + line("contents/b\\x.wav"))), "no key, or a key with a bad escape"},
            {"negative-size", gzip(sealed(Header + "t\tcontents/a.wav\t-10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\t-\t-\t-\n")), "size or time is not a decimal number"},
            {"revision-not-a-number", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\tx\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\t-\t-\t-\n")), "a revision is not a decimal number"},
            {"negative-revision", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t-1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\t30\t-\t-\t-\n")), "a revision is not a decimal number"},
            {"tempo-one-decimal", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.0\t180.00\t-\t-\t-\t0\t30\t-\t-\t-\n")), "tempo range is not two numbers"},
            {"tempo-exponent", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t7e1.00\t180.00\t-\t-\t-\t0\t30\t-\t-\t-\n")), "tempo range is not two numbers"},
            {"short-fingerprint", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t" + std::string(63, 'a') + "\t-\t-\t0\t30\t-\t-\t-\n")), "grid fingerprint is neither"},
            {"uppercase-fingerprint", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t" + std::string(64, 'A') + "\t-\t0\t30\t-\t-\t-\n")), "grid fingerprint is neither"},
            {"empty-key-name", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t\t0\t30\t-\t-\t-\n")), "the key is empty or has a bad escape"},
            {"key-written-without-key", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t1\t30\t-\t-\t-\n")), "keyWritten is not 0 or 1"},
            {"key-written-not-a-bool", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\tAm\t2\t30\t-\t-\t-\n")), "keyWritten is not 0 or 1"},
            {"time-not-a-number", gzip(sealed(Header + "t\tcontents/a.wav\t10\t20\t1\t2\t1\t1\t0.3.0\t70.00\t180.00\t-\t-\t-\t0\tsoon\t-\t-\t-\n")), "time of the analysis is not a decimal number"},
            {"repeated-key", gzip(sealed(Header + line("contents/a.wav") + line("contents/a.wav"))), "a key listed twice"},
            {"out-of-order", gzip(sealed(Header + line("contents/z.wav") + line("contents/a.wav"))), "keys out of order"},
            {"blank-line", gzip(sealed(Header + GoodLine + "\n")), "unknown line kind \"\""},
            {"no-header", gzip(sealed(GoodLine)), "not a Seabass analysis ledger"},
            {"confidence-two-decimals", gzip(sealed(Header + LineToTime + "\t0.87\t0\t-\n")), "grid's confidence is neither"},
            {"confidence-over-one", gzip(sealed(Header + LineToTime + "\t1.001\t0\t-\n")), "grid's confidence is neither"},
            {"confidence-two-digits", gzip(sealed(Header + LineToTime + "\t10.000\t0\t-\n")), "grid's confidence is neither"},
            {"confidence-signed", gzip(sealed(Header + LineToTime + "\t+0.500\t0\t-\n")), "grid's confidence is neither"},
            {"confidence-comma", gzip(sealed(Header + LineToTime + "\t0,500\t0\t-\n")), "grid's confidence is neither"},
            {"follows-not-a-bool", gzip(sealed(Header + LineToTime + "\t0.500\t2\t-\n")), "gridFollowsTempo is neither"},
            {"follows-without-confidence", gzip(sealed(Header + LineToTime + "\t-\t1\t-\n")), "are not both"},
            {"confidence-without-follows", gzip(sealed(Header + LineToTime + "\t0.500\t-\t-\n")), "are not both"},
            {"margin-three-decimals", gzip(sealed(Header + LineToTime + "\t-\t-\t4.200\n")), "key margin is neither"},
            {"margin-negative", gzip(sealed(Header + LineToTime + "\t-\t-\t-4.20\n")), "key margin is neither"},
            {"margin-empty", gzip(sealed(Header + LineToTime + "\t-\t-\t\n")), "key margin is neither"},
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
        const auto refuseWith = [&](const std::string &what, auto change) {
            AnalysisLedger l;
            auto e = entry("contents/b.wav", 'a');
            change(e);
            l[e.pathKey] = e;
            refused.emplace_back(what, l);
        };
        refuseWith("a confidence with two decimals", [](AnalysisLedgerEntry &e) { e.gridConfidence = "0.87"; });
        refuseWith("a confidence over 1", [](AnalysisLedgerEntry &e) { e.gridConfidence = "1.500"; });
        refuseWith("a confidence of \"-\" instead of empty", [](AnalysisLedgerEntry &e) {
            e.gridConfidence = "-";
            e.gridFollowsTempo = "-";
        });
        refuseWith("gridFollowsTempo not 0 or 1", [](AnalysisLedgerEntry &e) { e.gridFollowsTempo = "yes"; });
        refuseWith("gridFollowsTempo without a confidence", [](AnalysisLedgerEntry &e) { e.gridConfidence.clear(); });
        refuseWith("a confidence without gridFollowsTempo", [](AnalysisLedgerEntry &e) { e.gridFollowsTempo.clear(); });
        refuseWith("a key margin with one decimal", [](AnalysisLedgerEntry &e) { e.keyMargin = "4.2"; });
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

    // 5b. A grid's confidence with three decimals, a key's margin with
    //     two, built from integers.
    {
        check(formatGridConfidence(0.0) == "0.000", "0 is 0.000");
        check(formatGridConfidence(1.0) == "1.000", "1 is 1.000");
        check(formatGridConfidence(0.8754) == "0.875", "0.8754 is 0.875, got " + formatGridConfidence(0.8754));
        check(formatGridConfidence(0.0396) == "0.040", "0.0396 is 0.040, got " + formatGridConfidence(0.0396));
        check(formatGridConfidence(0.9996) == "1.000", "0.9996 is 1.000");
        check(formatGridConfidence(-0.001).empty() && formatGridConfidence(1.001).empty()
                  && formatGridConfidence(std::numeric_limits<double>::quiet_NaN()).empty(),
              "a confidence outside 0 to 1, or not a number, has no text");
        check(formatKeyMargin(0.0) == "0.00", "a margin of 0 is 0.00");
        check(formatKeyMargin(4.2) == "4.20", "4.2 is 4.20, got " + formatKeyMargin(4.2));
        check(formatKeyMargin(0.005) == "0.01", "0.005 is 0.01, got " + formatKeyMargin(0.005));
        check(formatKeyMargin(123.456) == "123.46", "123.456 is 123.46");
        check(formatKeyMargin(-0.01).empty() && formatKeyMargin(std::numeric_limits<double>::infinity()).empty(),
              "a negative margin, or one that is not a number, has no text");
        // What they write is what the ledger takes.
        AnalysisLedgerEntry e = entry("contents/a.wav", 'a');
        e.gridConfidence = formatGridConfidence(0.333333);
        e.keyMargin = formatKeyMargin(17.0);
        check(analysisLedgerEntryProblem(e.pathKey, e).empty(), "their text is written as it is");
        std::cout << "case 5b (confidence and margin text) done\n";
    }

    // 6. A read that stops short of the file's size: the file could not be
    //    read, which is not damage, and no save may replace it. (On libc++
    //    and MSVC a stream that fails part way reads as the end of the
    //    file; the hook stands in for that.)
    {
        const fs::path stick = freshStick("short-read");
        AnalysisLedger ledger;
        ledger["contents/a.wav"] = entry("contents/a.wav", 'a');
        std::string error;
        check(writeAnalysisLedger(stick, ledger, NoHook, &error), "the ledger is written: " + error);
        setAfterAnalysisLedgerReadForTesting([](std::string &bytes) { bytes.resize(bytes.size() / 2); });
        AnalysisLedgerFault fault = AnalysisLedgerFault::None;
        const AnalysisLedger read = readAnalysisLedger(stick, &error, &fault);
        setAfterAnalysisLedgerReadForTesting({});
        check(read.empty() && fault == AnalysisLedgerFault::Unreadable,
              "a read that stopped short cannot be read, not damaged, got \"" + error + "\"");
        check(error.find("analysis.tsv.gz") != std::string::npos && error.find("reading it stopped after") != std::string::npos,
              "and says so, got \"" + error + "\"");
        check(readAnalysisLedger(stick, &error, &fault) == ledger && fault == AnalysisLedgerFault::None,
              "read whole, it is the ledger written");
        std::cout << "case 6 (a read that stops short: cannot be read, not damaged) done\n";
    }

    // 7. A file larger than any ledger compresses to: damaged, refused for
    //    its size before it is read. A sparse file, so the test takes no
    //    room.
    {
        const fs::path stick = freshStick("too-large");
        putBytes(stick, "");
        const fs::path file = infrastructure::paths::stickAnalysisLedger(stick);
        const std::uintmax_t cap = 64u * 1024u * 1024u + 1024u * 1024u;
        bool asked = false;
        setAfterAnalysisLedgerReadForTesting([&asked](std::string &) { asked = true; });
        fs::resize_file(file, cap + 1);
        std::string error;
        AnalysisLedgerFault fault = AnalysisLedgerFault::None;
        check(readAnalysisLedger(stick, &error, &fault).empty() && fault == AnalysisLedgerFault::Damaged
                  && error.find(std::to_string(cap + 1) + " bytes, more than a ledger can be") != std::string::npos,
              "a file past the compressed cap is damaged for its size, got \"" + error + "\"");
        check(!asked, "and is not read");
        // At the cap it is read (and is not gzip).
        fs::resize_file(file, cap);
        check(readAnalysisLedger(stick, &error, &fault).empty() && fault == AnalysisLedgerFault::Damaged
                  && error.find("more than a ledger can be") == std::string::npos && asked,
              "a file at the cap is read, got \"" + error + "\"");
        setAfterAnalysisLedgerReadForTesting({});
        fs::remove(file);
        std::cout << "case 7 (a file past the compressed cap: damaged, not read) done\n";
    }

    // 8. A ledger that can be read and not written: a folder where the
    //    atomic write puts its temporary file. The write fails, says so,
    //    and the old file stays as it was.
    {
        const fs::path stick = freshStick("not-writable");
        AnalysisLedger ledger;
        ledger["contents/a.wav"] = entry("contents/a.wav", 'a');
        std::string error;
        check(writeAnalysisLedger(stick, ledger, NoHook, &error), "the first ledger is written: " + error);
        const fs::path file = infrastructure::paths::stickAnalysisLedger(stick);
        const std::string before = readBytes(file);
        fs::path temporary = file;
        temporary += ".tmp-seabass-write";
        fs::create_directories(temporary / "in the way");
        AnalysisLedger more = ledger;
        more["contents/b.wav"] = entry("contents/b.wav", 'b');
        bool hooked = false;
        check(!writeAnalysisLedger(stick, more, [&](const std::string &) { hooked = true; }, &error)
                  && error.find("analysis.tsv.gz") != std::string::npos && error.find("the write failed") != std::string::npos,
              "a write that cannot be made fails, naming the file, got \"" + error + "\"");
        check(hooked, "the hook was called before the write was tried");
        check(readBytes(file) == before && readAnalysisLedger(stick, &error) == ledger, "the old file stays as it was");
        fs::remove_all(temporary);
        check(writeAnalysisLedger(stick, more, NoHook, &error) && readAnalysisLedger(stick, &error) == more,
              "with the folder gone it is written");
        std::cout << "case 8 (a ledger that can be read and not written: the old one stays) done\n";
    }

    // 9. Moving a ledger that cannot be used aside: renamed in its folder
    //    with the UTC time, a number added when that name is taken; never
    //    deleted; none there is an error.
    {
        const fs::path stick = freshStick("aside");
        const fs::path file = infrastructure::paths::stickAnalysisLedger(stick);
        std::string error;
        check(!moveAnalysisLedgerAside(stick, std::chrono::system_clock::now(), &error)
                  && error.find("there is none to move aside") != std::string::npos,
              "no ledger: nothing moved, got \"" + error + "\"");
        // A folder in the file's place: what cannot be read is moved whole.
        fs::create_directories(file / "in the way");
        const auto at = std::chrono::sys_days{std::chrono::year{2026} / 11 / 26} + std::chrono::hours{14}
                        + std::chrono::minutes{3};
        const auto moved = moveAnalysisLedgerAside(stick, at, &error);
        check(moved && *moved == file.parent_path() / "analysis.tsv.gz.unreadable-20261126T140300Z",
              "renamed with the UTC time in the same folder, got \"" + (moved ? pathToUtf8(*moved) : error) + "\"");
        check(!fs::exists(file) && moved && fs::is_directory(*moved / "in the way"), "moved whole, nothing deleted");
        putBytes(stick, "damaged");
        const auto again = moveAnalysisLedgerAside(stick, at, &error);
        check(again && *again == file.parent_path() / "analysis.tsv.gz.unreadable-20261126T140300Z-2"
                  && readBytes(*again) == "damaged",
              "a second in the same second is numbered, got \"" + (again ? pathToUtf8(*again) : error) + "\"");
        check(readAnalysisLedger(stick, &error).empty() && error.empty(), "and the stick has no ledger now");
        std::cout << "case 9 (a ledger moved aside, never deleted) done\n";
    }

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "analysis_ledger_file_test: all passed\n";
    return 0;
}
