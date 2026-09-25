// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A backup can carry a name the person gave it, and the manifest is
// where it lives.
//
// The grammar grows by one optional trailing field, which is how it grew
// twice before (libraryFingerprint, then sourceReadOnly). That only works
// if old headers keep parsing: backups written by earlier builds are
// sitting on real sticks right now, and this project is pre-1.0 about its
// own code but not about data already written. So every shorter header
// this format has ever produced is checked here, not just the newest.
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>

#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"

using seabass::infrastructure::stick_backup::BackupManifest;
using seabass::infrastructure::stick_backup::BackupStatus;
using seabass::infrastructure::stick_backup::ManifestRow;

namespace
{

// The manifest is hash-trailed, so editing a header by hand means
// recomputing the trailer -- otherwise every case below would fail for
// the hash rather than for the thing being tested.
std::string withTrailer(const std::string &body)
{
    namespace hashing = seabass::infrastructure::hashing;
    return body + "#sha256\t" + hashing::toHex(hashing::Sha256::of(body)) + "\n";
}

BackupManifest basicManifest()
{
    BackupManifest manifest;
    manifest.stickIdentifier = "1234-ABCD";
    manifest.stickLabel = "WHALESHARK";
    manifest.status = BackupStatus::Complete;
    manifest.createdAtUnix = 1700000000;
    return manifest;
}

// Re-serialising and re-parsing is the only honest check: it proves the
// writer and the reader agree, rather than proving one of them matches a
// string this test made up.
BackupManifest roundTrip(const BackupManifest &manifest)
{
    std::string error;
    auto parsed = BackupManifest::parse(manifest.serialize(), &error);
    assert(parsed && error.empty());
    return *parsed;
}

void testNameSurvivesARoundTrip()
{
    BackupManifest manifest = basicManifest();
    manifest.userName = "before the Berlin gig";

    const BackupManifest parsed = roundTrip(manifest);
    assert(parsed.userName == "before the Berlin gig");
    assert(parsed.stickLabel == "WHALESHARK");
    assert(parsed.createdAtUnix == 1700000000);
}

void testNoNameIsEmptyNotMissing()
{
    const BackupManifest parsed = roundTrip(basicManifest());
    assert(parsed.userName.empty());
}

void testNameEscapesTheCharactersThatWouldBreakTheGrammar()
{
    // Tab and newline are the row and field separators, backslash is the
    // escape itself. A name is free text typed by a person, so all three
    // have to survive rather than be rejected.
    BackupManifest manifest = basicManifest();
    manifest.userName = "before\tthe\nBerlin \\ gig";

    const BackupManifest parsed = roundTrip(manifest);
    assert(parsed.userName == "before\tthe\nBerlin \\ gig");
}

void testNameDoesNotDisturbTheFieldsBesideIt()
{
    BackupManifest manifest = basicManifest();
    manifest.libraryFingerprint = "fingerprint-here";
    manifest.sourceReadOnly = true;
    manifest.userName = "emergency copy";

    const BackupManifest parsed = roundTrip(manifest);
    assert(parsed.libraryFingerprint == "fingerprint-here");
    assert(parsed.sourceReadOnly);
    assert(parsed.userName == "emergency copy");
}

// The older shapes, spelled out. Each of these is on a real stick
// somewhere and must keep opening.
void testOlderHeadersStillParse()
{
    struct Case
    {
        const char *what;
        std::string header;
    };

    const Case cases[] = {
        {"6 fields: before the library fingerprint existed",
          "seabass-stick-manifest\t1\t1234-ABCD\tWHALESHARK\tcomplete\t1700000000"},
        {"7 fields: fingerprint, no read-only flag",
          "seabass-stick-manifest\t1\t1234-ABCD\tWHALESHARK\tcomplete\t1700000000\tfp"},
        {"8 fields: read-only flag, no name (what every backup written before today looks like)",
          "seabass-stick-manifest\t1\t1234-ABCD\tWHALESHARK\tcomplete\t1700000000\tfp\t0"},
    };

    for (const Case &entry : cases) {
        // Build a whole document so the trailer hash is right; the parser
        // verifies it, and a header-only string would fail for that
        // reason rather than the one under test.
        BackupManifest source = basicManifest();
        std::string document = source.serialize();
        const std::string::size_type firstNewline = document.find('\n');
        assert(firstNewline != std::string::npos);
        document.replace(0, firstNewline, entry.header);

        // Rehash, since the header changed.
        const std::string::size_type trailer = document.rfind("#sha256");
        assert(trailer != std::string::npos);
        document.erase(trailer);

        std::string error;
        auto parsed = BackupManifest::parse(withTrailer(document), &error);
        if (!parsed) {
            std::cerr << "failed on " << entry.what << ": " << error << "\n";
        }
        assert(parsed && error.empty());
        assert(parsed->stickLabel == "WHALESHARK");
        assert(parsed->userName.empty() && "an old backup simply has no name");
    }
}

}  // namespace

// A salvage read stops part-way, so the archive holds less of the file
// than the stick had. Both numbers have to survive: what is stored, so a
// restore knows what to write, and what the file was, so nothing
// presents a truncated track as a whole one.
void testASalvagedRowKeepsBothSizes()
{
    BackupManifest manifest = basicManifest();
    ManifestRow whole;
    whole.kind = ManifestRow::Kind::File;
    whole.path = "Contents/whole.mp3";
    whole.size = 9'000'000;
    whole.mtimeUnix = 1700000001;
    ManifestRow salvaged;
    salvaged.kind = ManifestRow::Kind::File;
    salvaged.path = "Contents/damaged.mp3";
    salvaged.size = 4'194'304;            // what was readable
    salvaged.salvagedFromSize = 9'000'000;  // what the stick said it was
    salvaged.mtimeUnix = 1700000002;
    manifest.rows = {whole, salvaged};

    const BackupManifest parsed = roundTrip(manifest);
    assert(parsed.rows.size() == 2);
    assert(parsed.rows[0].salvagedFromSize == 0 && "a whole file says nothing about salvage");
    assert(parsed.rows[1].size == 4'194'304);
    assert(parsed.rows[1].salvagedFromSize == 9'000'000);
    assert(parsed.rows[1].path == "Contents/damaged.mp3" && "the field beside it is still itself");
}

// Every row a healthy stick writes stays seven fields wide, so an
// archive that has never met a damaged stick serializes exactly as it
// did before this field existed. Checked by counting tabs rather than
// by trusting the round trip, which would agree with a writer that
// always wrote eight.
void testAWholeRowIsStillSevenFields()
{
    BackupManifest manifest = basicManifest();
    ManifestRow row;
    row.kind = ManifestRow::Kind::File;
    row.path = "Contents/whole.mp3";
    row.size = 12;
    row.mtimeUnix = 1700000001;
    manifest.rows = {row};

    const std::string document = manifest.serialize();
    std::size_t lineStart = document.find("f\tContents/whole.mp3");
    assert(lineStart != std::string::npos);
    const std::size_t lineEnd = document.find('\n', lineStart);
    const std::string line = document.substr(lineStart, lineEnd - lineStart);
    assert(std::count(line.begin(), line.end(), '\t') == 6 && "seven fields, six tabs");

    manifest.rows[0].salvagedFromSize = 99;
    const std::string salvagedDocument = manifest.serialize();
    lineStart = salvagedDocument.find("f\tContents/whole.mp3");
    const std::size_t salvagedEnd = salvagedDocument.find('\n', lineStart);
    const std::string salvagedLine = salvagedDocument.substr(lineStart, salvagedEnd - lineStart);
    assert(std::count(salvagedLine.begin(), salvagedLine.end(), '\t') == 7 && "eight fields, seven tabs");
}

// A manifest written before the field existed has seven-field rows and
// must still parse, because it is sitting on real sticks right now.
void testSevenFieldRowsStillParse()
{
    std::string body = "seabass-stick-manifest\t1\t1234-ABCD\tWHALESHARK\tcomplete\t1700000000\n";
    body += "f\tContents/a.mp3\t12\t1700000001\t00000000\t";
    body += std::string(64, '0');
    body += "\t\n";

    std::string error;
    auto parsed = BackupManifest::parse(withTrailer(body), &error);
    assert(parsed && error.empty());
    assert(parsed->rows.size() == 1);
    assert(parsed->rows[0].salvagedFromSize == 0 && "an old row is a whole row, not an unknown one");
}

int main()
{
    testASalvagedRowKeepsBothSizes();
    testAWholeRowIsStillSevenFields();
    testSevenFieldRowsStillParse();
    testNameSurvivesARoundTrip();
    testNoNameIsEmptyNotMissing();
    testNameEscapesTheCharactersThatWouldBreakTheGrammar();
    testNameDoesNotDisturbTheFieldsBesideIt();
    testOlderHeadersStillParse();
    std::cout << "backup_manifest_name_test passed\n";
    return 0;
}
