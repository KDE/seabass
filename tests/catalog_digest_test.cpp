// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A catalog digest says what the catalog SAYS, not how the file says it.
//
// The shakedown rig compares catalogs by sha256 over their bytes, and
// that cannot tell "SQLite folded the write-ahead log into the database"
// from "the library changed": a checkpoint rewrites the file, so both
// look the same to a byte comparison. Windows round 7's F4-undo failed
// on exactly that and the round could only report the difference, never
// explain it. exportLibrary.db is SQLCipher-encrypted, so neither
// sha256sum nor sqlite3 can read it to find out -- only Seabass can.
//
// The case that matters is case 2, and it is the undo's shape: write a
// cue, then write the original set back. The library ends where it
// started and the FILE does not, which is what a rig comparing bytes
// reports as a changed catalog. Case 3 is there so case 2 cannot pass by
// the digest being blind to everything.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "application/catalog_digest.hpp"
#include "domain/track.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::domain;
namespace fs = std::filesystem;

namespace
{

fs::path freshCopy(const std::string &name)
{
    const fs::path scratch = seabass::testing::scratchRoot() / name;
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    const fs::path source = fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox";
    assert(fs::exists(source / "rekordbox" / "exportLibrary.db"));
    const fs::path pioneerRoot = scratch / "PIONEER";
    fs::copy(source, pioneerRoot, fs::copy_options::recursive);
    return pioneerRoot;
}

std::string digestOf(const fs::path &pioneerRoot)
{
    infrastructure::onelibrary::OneLibraryReader reader(pioneerRoot.string());
    return application::catalogDigest(reader.readAll());
}

// What the rig does today: sha256 over the database file's bytes.
std::string bytesOf(const fs::path &pioneerRoot)
{
    const fs::path db = pioneerRoot / "rekordbox" / "exportLibrary.db";
    std::ifstream in(db, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(in)), {});
    return infrastructure::hashing::toHex(infrastructure::hashing::Sha256::of(contents));
}

// A track the mirror lists and that already carries cues, so writing the
// same set back is a real round trip rather than a no-op on an empty one.
Track pickTrack(const fs::path &pioneerRoot)
{
    infrastructure::onelibrary::OneLibraryReader reader(pioneerRoot.string());
    for (const Track &track : reader.readAll()) {
        if (!track.filePath.empty() && !track.cues.empty()) {
            return track;
        }
    }
    return {};
}

}  // namespace

int main()
{
    // ---- 1: the same catalog, read twice, digests the same ------------
    {
        const fs::path root = freshCopy("seabass_digest_stable");
        const std::string a = digestOf(root);
        const std::string b = digestOf(root);
        assert(a == b && "two reads of one catalog must agree, or nothing below means anything");
        assert(a.size() == 64);
        std::cout << "case 1 (stable across reads) OK: " << a.substr(0, 16) << "...\n";
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
    }

    // ---- 2: the undo's shape -- bytes move, content does not ----------
    {
        const fs::path root = freshCopy("seabass_digest_roundtrip");
        const Track track = pickTrack(root);
        assert(!track.filePath.empty() && "the fixture must list a track with cues in Device Library Plus");

        const std::string digestBefore = digestOf(root);
        const std::string bytesBefore = bytesOf(root);

        {
            infrastructure::onelibrary::OneLibraryCueWriter writer(root.string());
            std::vector<CuePoint> changed = track.cues;
            CuePoint extra;
            extra.kind = CuePoint::Kind::Memory;
            extra.positionMs = 31337.0;
            changed.push_back(extra);
            writer.writeCuesForPath(track.filePath, changed);
        }
        assert(digestOf(root) != digestBefore && "the interim write must actually change the catalog");

        {
            // Back to exactly what was there, which is what an undo does.
            infrastructure::onelibrary::OneLibraryCueWriter writer(root.string());
            writer.writeCuesForPath(track.filePath, track.cues);
        }

        const std::string digestAfter = digestOf(root);
        const std::string bytesAfter = bytesOf(root);

        if (digestAfter != digestBefore) {
            std::cerr << "the digest moved although the library came back:\n  " << digestBefore << "\n  "
                      << digestAfter << "\n";
        }
        assert(digestAfter == digestBefore && "a library restored to what it was must digest the same");

        // And the thing the rig would have reported. Not asserted as a
        // guarantee -- SQLite is free to land on the same bytes -- but
        // said out loud, because if it ever stops differing then this
        // test has stopped covering the case it was written for.
        if (bytesAfter == bytesBefore) {
            std::cout << "  note: the FILE came back byte-identical this run, so the byte comparison would\n"
                         "        have passed too and this case did not exercise the difference\n";
        } else {
            std::cout << "  the file's bytes differ (" << bytesBefore.substr(0, 12) << "... -> "
                      << bytesAfter.substr(0, 12) << "...), which is what the rig reports as a changed catalog\n";
        }
        std::cout << "case 2 (a restored library digests the same however the file moved) OK\n";
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
    }

    // ---- 3: a real change is still seen -------------------------------
    {
        const fs::path root = freshCopy("seabass_digest_sees_change");
        const Track track = pickTrack(root);
        assert(!track.filePath.empty());
        const std::string before = digestOf(root);
        {
            infrastructure::onelibrary::OneLibraryCueWriter writer(root.string());
            std::vector<CuePoint> changed = track.cues;
            CuePoint extra;
            extra.kind = CuePoint::Kind::Memory;
            extra.positionMs = 4242.0;
            changed.push_back(extra);
            writer.writeCuesForPath(track.filePath, changed);
        }
        assert(digestOf(root) != before && "one added cue must move the digest, or case 2 proves nothing");
        std::cout << "case 3 (one added cue moves it) OK\n";
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
    }

    std::cout << "catalog_digest_test passed\n";
    return 0;
}
