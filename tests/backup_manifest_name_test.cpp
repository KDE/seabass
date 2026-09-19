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
#include <cassert>
#include <iostream>
#include <string>

#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"

using seabass::infrastructure::stick_backup::BackupManifest;
using seabass::infrastructure::stick_backup::BackupStatus;

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
        {"8 fields: read-only flag, no name -- what every backup written before today looks like",
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

int main()
{
    testNameSurvivesARoundTrip();
    testNoNameIsEmptyNotMissing();
    testNameEscapesTheCharactersThatWouldBreakTheGrammar();
    testNameDoesNotDisturbTheFieldsBesideIt();
    testOlderHeadersStillParse();
    std::cout << "backup_manifest_name_test passed\n";
    return 0;
}
