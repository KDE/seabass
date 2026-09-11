// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Opens archives written by Zip64Writer with implementations that share
// no code with ours -- Python's zipfile (always, when python3 exists),
// Info-ZIP's unzip and 7-Zip (when installed). Our own reader must never
// be the only thing grading our writer's homework: a writer and reader
// with the same author can agree with each other and both disagree with
// the ZIP specification.
//
// Skipping is the dangerous state for a test like this, because a skip
// and a pass look the same in every summary. So the tools are looked for
// three ways -- the environment (ctest supplies absolute paths), the path
// CMake found at configure time, and finally the bare name on PATH -- and
// a tool the build was configured with is a **failure** when it cannot be
// run, never a skip. Exit 77 (ctest SKIP_RETURN_CODE) is reserved for the
// one honest case: a machine that genuinely has no python3, where the
// dependency is absent rather than broken.
//
// Previously the python path came only from ctest's ENVIRONMENT property,
// so running the binary directly -- by hand, or from a CI lane that
// invokes tests rather than ctest -- skipped every case on a machine with
// python3 sitting in /bin.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::stick_backup;
namespace fs = std::filesystem;

namespace
{

std::string envOrEmpty(const char *name)
{
    const char *value = std::getenv(name);
    if (value == nullptr) {
        return {};
    }
    std::string s(value);
    if (s.size() >= 8 && s.compare(s.size() - 8, 8, "NOTFOUND") == 0) {
        return {};
    }
    return s;
}

// What CMake found when this build was configured. Empty when it found
// nothing. Baked in so the binary can tell "this machine has no python"
// apart from "this build expected python and something has since moved",
// which are the same symptom and very different bugs.
#ifndef SEABASS_CONFIGURED_PYTHON3
#define SEABASS_CONFIGURED_PYTHON3 ""
#endif
#ifndef SEABASS_CONFIGURED_UNZIP
#define SEABASS_CONFIGURED_UNZIP ""
#endif
#ifndef SEABASS_CONFIGURED_SEVENZIP
#define SEABASS_CONFIGURED_SEVENZIP ""
#endif

std::string configured(const char *value)
{
    std::string s(value);
    if (s.size() >= 8 && s.compare(s.size() - 8, 8, "NOTFOUND") == 0) {
        return {};
    }
    return s;
}

#if defined(_WIN32)
constexpr const char *kNullDevice = "NUL";
#else
constexpr const char *kNullDevice = "/dev/null";
#endif

std::string shellQuote(const std::string &s)
{
    return "\"" + s + "\"";
}

// Runs a command, returns {exit status, stdout}.
std::pair<int, std::string> run(const std::string &command)
{
#if defined(_WIN32)
    // _popen runs the command through `cmd /c`, which applies its own
    // quote-stripping rule first: when the command line begins with a
    // quote, cmd removes the FIRST and LAST quote characters of the whole
    // string. Every command built here starts with a quoted program path
    // and ends with a quoted argument, so that rule mangles both ends at
    // once, leaving cmd with an unbalanced line and failing before the
    // program ever runs ("The filename, directory name, or volume label
    // syntax is incorrect"). The test then read that as the archive being
    // rejected, when python had in fact never been invoked -- verified by
    // running the identical command by hand, which lists the archive fine.
    //
    // Wrapping the whole thing in one more pair of quotes is cmd's own
    // documented escape hatch: the outer pair is what gets stripped, and
    // the real command survives intact.
    const std::string wrapped = "\"" + command + "\"";
    FILE *pipe = _popen(wrapped.c_str(), "r");
#else
    FILE *pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return {-1, {}};
    }
    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) {
        output += buffer;
    }
#if defined(_WIN32)
    int status = _pclose(pipe);
#else
    int status = pclose(pipe);
    if (status != -1) {
        status = WEXITSTATUS(status);
    }
#endif
    return {status, output};
}

// Finds a tool the three ways it can be known about, most specific
// first, and proves the answer by running it -- a name that resolves and
// a name that works are different things, and the difference is exactly
// what silently emptied this test before.
std::string locateTool(const char *envName, const std::string &configuredPath,
                       const std::vector<std::string> &candidates, const std::string &probeArgs)
{
    std::vector<std::string> tries;
    if (std::string fromEnv = envOrEmpty(envName); !fromEnv.empty()) {
        tries.push_back(fromEnv);
    }
    if (!configuredPath.empty()) {
        tries.push_back(configuredPath);
    }
    tries.insert(tries.end(), candidates.begin(), candidates.end());

    for (const std::string &candidate : tries) {
        auto [status, output] = run(shellQuote(candidate) + " " + probeArgs + " >" + kNullDevice + " 2>&1");
        (void)output;
        if (status == 0) {
            return candidate;
        }
    }
    return {};
}

// Reports a tool that the build was configured with and that cannot be
// run now. That is a broken build rather than an absent dependency, so
// it must fail: skipping here is how a green suite stops checking
// anything.
bool refuseIfConfiguredButMissing(const char *label, const std::string &configuredPath, const std::string &located)
{
    if (!located.empty() || configuredPath.empty()) {
        return false;
    }
    std::cerr << "FAILED: this build was configured with " << label << " at " << configuredPath
              << ", and it cannot be run now. Refusing to skip: a cross-validation test that\n"
                 "silently validates nothing is worse than one that is absent.\n";
    return true;
}

std::string pseudoRandom(std::size_t size, std::uint64_t seed)
{
    std::string out(size, '\0');
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
    for (std::size_t i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = static_cast<char>(x & 0xff);
    }
    return out;
}

// name -> (size, crc) as Python sees it. The script is written to a file
// so no shell quoting of Python source is involved.
// Text-shaped, so deflate has something to do. pseudoRandom() above is
// deliberately incompressible and would prove nothing here.
std::string compressibleText(std::size_t size)
{
    static const std::string unit = "memory cue at 0:00 for track ";
    std::string out;
    while (out.size() < size) {
        out += unit + std::to_string(out.size());
    }
    out.resize(size);
    return out;
}

std::map<std::string, std::pair<std::uint64_t, std::uint32_t>> pythonListing(const std::string &python, const fs::path &script,
                                                                           const fs::path &archive)
{
    auto [status, output] = run(shellQuote(python) + " " + shellQuote(script.string()) + " " + shellQuote(archive.string()));
    if (status != 0) {
        std::cerr << "python zipfile rejected " << archive << ":\n" << output << "\n";
        assert(false && "python zipfile rejected the archive");
    }
    std::map<std::string, std::pair<std::uint64_t, std::uint32_t>> listing;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        std::size_t t1 = line.rfind('\t');
        std::size_t t2 = line.rfind('\t', t1 - 1);
        assert(t1 != std::string::npos && t2 != std::string::npos);
        std::string name = line.substr(0, t2);
        std::uint64_t size = std::stoull(line.substr(t2 + 1, t1 - t2 - 1));
        std::uint32_t crc = static_cast<std::uint32_t>(std::stoul(line.substr(t1 + 1)));
        listing[name] = {size, crc};
    }
    return listing;
}

void compareWithOurReader(const fs::path &archive,
                          const std::map<std::string, std::pair<std::uint64_t, std::uint32_t>> &theirs)
{
    PosixArchiveFile file(archive, PosixArchiveFile::OpenMode::ReadOnly);
    Zip64Reader reader = Zip64Reader::open(file);
    assert(theirs.size() == reader.entries().size());
    for (const CentralEntry &entry : reader.entries()) {
        auto it = theirs.find(entry.name);
        if (it == theirs.end()) {
            std::cerr << "python did not list " << entry.name << "\n";
            assert(false);
        }
        assert(it->second.first == entry.size);
        assert(it->second.second == entry.crc32);
    }
}

}  // namespace

int main()
{
    const std::string configuredPython = configured(SEABASS_CONFIGURED_PYTHON3);
    const std::string configuredUnzip = configured(SEABASS_CONFIGURED_UNZIP);
    const std::string configuredSevenZip = configured(SEABASS_CONFIGURED_SEVENZIP);

    const std::string python = locateTool("SEABASS_PYTHON3", configuredPython, {"python3", "python"}, "--version");
    const std::string unzip = locateTool("SEABASS_UNZIP", configuredUnzip, {"unzip"}, "-v");
    const std::string sevenZip = locateTool("SEABASS_SEVENZIP", configuredSevenZip, {"7z", "7zz", "7za"}, "i");

    bool broken = refuseIfConfiguredButMissing("python3", configuredPython, python);
    broken = refuseIfConfiguredButMissing("unzip", configuredUnzip, unzip) || broken;
    broken = refuseIfConfiguredButMissing("7-Zip", configuredSevenZip, sevenZip) || broken;
    if (broken) {
        return 1;
    }
    // Unreachable from a Seabass build: python3, unzip and 7z are all
    // required at configure time (see the SEABASS_TESTS block in
    // CMakeLists.txt), so a binary that exists was configured with all
    // three, and a configured tool that has since gone is caught by
    // refuseIfConfiguredButMissing() above.
    //
    // It stays as a failure rather than a skip because this test is the
    // only thing stopping our ZIP writer and our ZIP reader agreeing with
    // each other and both disagreeing with the specification. "Graded
    // only by our own reader" is not a weaker pass, it is no check at
    // all, and a run that checks nothing must not report success.
    if (python.empty()) {
        std::cerr << "FAIL: no python3, so the archive would be graded only by our own reader.\n"
                     "This build requires python3 at configure time, so reaching here means the\n"
                     "binary outlived its build tree. Reconfigure, or build with\n"
                     "-DSEABASS_TESTS=OFF if you want no test suite at all.\n";
        return 1;
    }
    std::cout << "using python3=" << python << " unzip=" << (unzip.empty() ? "(none)" : unzip)
              << " 7z=" << (sevenZip.empty() ? "(none)" : sevenZip) << "\n";

    fs::path root = seabass::testing::scratchRoot() / "seabass_backup_archive_crossvalidation_test";
    fs::remove_all(root);
    fs::create_directories(root);

    fs::path script = root / "listing.py";
    {
        std::ofstream out(script);
        // Entry names come back over a pipe, and Python encodes a
        // non-tty stdout with locale.getpreferredencoding() -- the ANSI
        // code page, cp1252 on this machine. "Contents/Cafe del Mar.mp3"
        // (with the accent) then arrives as cp1252 bytes while the name
        // read out of the archive is UTF-8, and comparing the two says
        // "python did not list" a file python listed perfectly well.
        // Nothing is wrong with the archive when that happens: testzip()
        // has already passed by then. Pin the encoding so the comparison
        // is about the archive rather than about the machine's locale.
        out << "import sys, zipfile\n"
               "sys.stdout.reconfigure(encoding='utf-8')\n"
               "z = zipfile.ZipFile(sys.argv[1])\n"
               "bad = z.testzip()\n"
               "if bad is not None:\n"
               "    print('testzip failed on', bad)\n"
               "    sys.exit(2)\n"
               "for i in z.infolist():\n"
               "    print(i.filename, i.file_size, i.CRC, sep='\\t')\n";
    }

    // Archive 1: the realistic shape -- nested dirs, unicode, empty file,
    // a few MB, a manifest.
    fs::path realistic = root / "realistic.zip";
    {
        PosixArchiveFile file(realistic, PosixArchiveFile::OpenMode::ReadWrite);
        Zip64Writer writer(file, {});
        writer.addDirectory("Contents", 1'700'000'000);
        writer.addDirectory("Contents/Amelie Lens", 1'700'000'001);
        writer.addFileFromMemory("Contents/Amelie Lens/whatever you do.mp3", 1'700'000'002, zip::bytesOf(pseudoRandom(5u << 20, 1)));
        writer.addFileFromMemory("Contents/Caf\xC3\xA9 del Mar.mp3", 1'700'000'003, zip::bytesOf(pseudoRandom(4096, 2)));
        writer.addFileFromMemory("Contents/empty.txt", 1'700'000'004, zip::bytesOf(""));
        writer.addDirectory("Engine Library/Music", 1'700'000'005);
        writer.addFileFromMemory("Engine Library/Database2/m.db", 1'700'000'006, zip::bytesOf(pseudoRandom(300'000, 3)));
        // Deflated entries go through the same gauntlet as stored ones.
        // Our writer's own comment claims every mainstream reader copes
        // with a streamed entry; this is what actually checks that, for
        // the compressed case as well as the stored one.
        writer.addFileFromMemory("PIONEER/USBANLZ/ANLZ0000.EXT", 1'700'000'007,
                                 zip::bytesOf(compressibleText(200'000)), nullptr, Compression::Deflate);
        writer.addFileFromMemory("PIONEER/USBANLZ/empty.EXT", 1'700'000'008, zip::bytesOf(""), nullptr,
                                 Compression::Deflate);
        BackupManifest manifest;
        manifest.stickIdentifier = "uuid";
        manifest.stickLabel = "WHALESHARK2";
        manifest.createdAtUnix = 1'700'000'007;
        writer.finish(manifest.serialize(), std::string(ManifestEntryName), manifest.createdAtUnix);
        file.barrier();
    }
    compareWithOurReader(realistic, pythonListing(python, script, realistic));
    std::cout << "case 1 (python zipfile: testzip clean, names/sizes/CRCs agree) OK\n";

    // Archive 2: forces the zip64 entry-count path in the *reader* under test.
    fs::path many = root / "many.zip";
    {
        PosixArchiveFile file(many, PosixArchiveFile::OpenMode::ReadWrite);
        Zip64Writer writer(file, {});
        for (std::size_t i = 0; i < 66'000; ++i) {
            writer.addFileFromMemory("n/" + std::to_string(i), 1, zip::bytesOf(i % 2 == 0 ? "" : "x"));
        }
        BackupManifest manifest;
        manifest.createdAtUnix = 1;
        writer.finish(manifest.serialize(), std::string(ManifestEntryName), 1);
        file.barrier();
    }
    compareWithOurReader(many, pythonListing(python, script, many));
    std::cout << "case 2 (python zipfile: 66 000 entries via the zip64 record) OK\n";

    if (!unzip.empty()) {
        auto [status, output] = run(shellQuote(unzip) + " -tqq " + shellQuote(realistic.string()) + " 2>&1");
        if (status != 0) {
            std::cerr << output << "\n";
        }
        assert(status == 0);
        std::cout << "case 3 (unzip -t) OK\n";
    } else {
        std::cout << "case 3 (unzip -t) skipped -- unzip not found\n";
    }

    if (!sevenZip.empty()) {
        auto [status, output] = run(shellQuote(sevenZip) + " t " + shellQuote(realistic.string()) + " 2>&1");
        if (status != 0) {
            std::cerr << output << "\n";
        }
        assert(status == 0);
        std::cout << "case 4 (7z t) OK\n";
    } else {
        std::cout << "case 4 (7z t) skipped -- 7z not found\n";
    }

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
