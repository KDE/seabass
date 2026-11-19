// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::findMovedFiles (#103): a missing row paired with a stray file
// by name and size, by name alone, by tags and length, and the shapes it
// must refuse to guess at.

#include <cassert>
#include <iostream>
#include <string>

#include "application/path_key.hpp"
#include "domain/moved_files.hpp"

using namespace seabass::domain;
using Confidence = MovedFileCandidate::Confidence;

namespace
{

Track row(std::string format, std::string id, std::string path, std::uint64_t catalogBytes, std::string title = {},
          std::string artist = {}, double duration = 0.0)
{
    Track t;
    t.format = std::move(format);
    t.sourceId = std::move(id);
    t.filePath = std::move(path);
    t.catalogFileBytes = catalogBytes;
    t.title = std::move(title);
    t.artist = std::move(artist);
    t.durationSeconds = duration;
    return t;
}

Track file(std::string path, std::uint64_t bytes, std::string title = {}, std::string artist = {},
           double duration = 0.0)
{
    Track t;
    t.format = "disk";
    t.isUnreferenced = true;
    t.sourceId = path;
    t.filePath = path;
    t.filename = path.substr(path.find_last_of('/') + 1);
    t.fileSizeBytes = bytes;
    t.title = std::move(title);
    t.artist = std::move(artist);
    t.durationSeconds = duration;
    return t;
}

const auto key = [](const std::string &path) { return seabass::application::normalizedPathKey(path); };

}  // namespace

int main()
{
    // 1. The same name and size under another folder: one finding, one
    //    high-confidence candidate, and the rows of every catalog that
    //    name the path are on it, however each spells the path.
    {
        const auto findings = findMovedFiles(
            {row("rekordbox", "7", "/S/Contents/Old/Track.mp3", 1000, "Track", "Someone", 200.0),
             row("onelibrary", "70", "/S/Contents/OLD/track.mp3", 0, "Track", "Someone", 200.0),
             row("engine", "700", "/S/Contents/Old/Track.mp3", 1000, "Track", "Someone", 200.0)},
            {file("/S/Contents/New/Track.mp3", 1000), file("/S/Contents/New/Other.mp3", 1000)}, key);
        assert(findings.size() == 1);
        assert(findings[0].rows.size() == 3);
        assert(findings[0].expectedPath == "/S/Contents/Old/Track.mp3");
        assert(findings[0].candidates.size() == 1);
        assert(findings[0].candidates[0].confidence == Confidence::High);
        assert(findings[0].candidates[0].file.filePath == "/S/Contents/New/Track.mp3");
        assert(findings[0].oneClick() && !findings[0].oneToConfirm());
        assert(findings[0].title() == "Track" && findings[0].artist() == "Someone");
        std::cout << "case 1 (same name and size is one high-confidence candidate, rows grouped by path) OK\n";
    }

    // 2. The same name with another size needs confirming, and so does
    //    the same name when no catalog recorded a size.
    {
        auto findings = findMovedFiles({row("rekordbox", "7", "/S/Contents/Old/Track.mp3", 1000)},
                                       {file("/S/Contents/New/Track.mp3", 1001)}, key);
        assert(findings.size() == 1 && findings[0].candidates.size() == 1);
        assert(findings[0].candidates[0].confidence == Confidence::NeedsConfirming);
        assert(findings[0].candidates[0].reason.find("1001") != std::string::npos);
        assert(findings[0].oneToConfirm() && !findings[0].oneClick());

        findings = findMovedFiles({row("engine", "7", "/S/Contents/Old/Track.mp3", 0)},
                                  {file("/S/Contents/New/Track.mp3", 1000)}, key);
        assert(findings.size() == 1 && findings[0].candidates.size() == 1);
        assert(findings[0].candidates[0].confidence == Confidence::NeedsConfirming);
        assert(findings[0].candidates[0].reason.find("does not record") != std::string::npos);
        std::cout << "case 2 (another size, or no recorded size, needs confirming) OK\n";
    }

    // 3. Another name, the same title, artist and length: needs
    //    confirming. The length has to be within the tolerance, and a
    //    file whose tags could not be read is not matched by tags.
    {
        auto findings = findMovedFiles(
            {row("rekordbox", "7", "/S/Contents/Old/01 Track.mp3", 1000, "Track", "Someone", 200.0)},
            {file("/S/Contents/New/Someone - Track.mp3", 1000, "Track", "Someone", 201.0),
             file("/S/Contents/New/Someone - Track (Extended).mp3", 1500, "Track", "Someone", 260.0),
             file("/S/Contents/New/untagged.mp3", 1000)},
            key, 2.0);
        assert(findings.size() == 1 && findings[0].candidates.size() == 1);
        assert(findings[0].candidates[0].file.filePath == "/S/Contents/New/Someone - Track.mp3");
        assert(findings[0].candidates[0].confidence == Confidence::NeedsConfirming);
        assert(findings[0].candidates[0].reason.find("same title, artist and length") != std::string::npos);
        std::cout << "case 3 (same tags and length under another name needs confirming) OK\n";
    }

    // 4. Two files of the name, or none: listed, never picked between.
    {
        auto findings = findMovedFiles({row("rekordbox", "7", "/S/Contents/Old/Track.mp3", 1000)},
                                       {file("/S/Contents/A/Track.mp3", 1000), file("/S/Contents/B/Track.mp3", 1000)},
                                       key);
        assert(findings.size() == 1 && findings[0].candidates.size() == 2);
        assert(findings[0].only() == nullptr && !findings[0].oneClick() && !findings[0].oneToConfirm());
        assert(findings[0].candidates[0].file.filePath == "/S/Contents/A/Track.mp3");  // by path within a tier

        findings = findMovedFiles({row("rekordbox", "7", "/S/Contents/Old/Track.mp3", 1000)},
                                  {file("/S/Contents/New/Else.mp3", 1000)}, key);
        assert(findings.size() == 1 && findings[0].candidates.empty());
        assert(findings[0].only() == nullptr);
        std::cout << "case 4 (several candidates or none is reported only) OK\n";
    }

    // 5. One file that two missing tracks could be goes back to one place
    //    only: high confidence for neither, and each says who else wants
    //    it.
    {
        auto findings = findMovedFiles({row("rekordbox", "7", "/S/Contents/Old/Track.mp3", 1000, "Track", "A"),
                                        row("rekordbox", "8", "/S/Contents/Older/Track.mp3", 1000, "Track", "B")},
                                       {file("/S/Contents/New/Track.mp3", 1000)}, key);
        assert(findings.size() == 2);
        for (const auto &finding : findings) {
            assert(finding.candidates.size() == 1);
            assert(finding.candidates[0].confidence == Confidence::NeedsConfirming);
            assert(finding.candidates[0].reason.find("also matches") != std::string::npos);
        }
        assert(findings[0].candidates[0].reason.find("\"Track\"") != std::string::npos);
        std::cout << "case 5 (a file two tracks claim is high confidence for neither) OK\n";
    }

    // 6. Rows without a path, and streaming rows, are nobody's finding;
    //    findings keep the order the rows came in; candidates are ranked
    //    high first.
    {
        Track streaming = row("engine", "9", "/cache/tidal/1.flac", 0);
        streaming.streamingSource = "tidal";
        auto findings = findMovedFiles({row("rekordbox", "1", "", 0), streaming,
                                        row("rekordbox", "2", "/S/Contents/Old/B.mp3", 500),
                                        row("rekordbox", "3", "/S/Contents/Old/A.mp3", 100)},
                                       {file("/S/Contents/New/A.mp3", 100), file("/S/Contents/New/B.mp3", 500),
                                        file("/S/Contents/Other/B.mp3", 501)},
                                       key);
        assert(findings.size() == 2);
        assert(findings[0].expectedPath == "/S/Contents/Old/B.mp3");
        assert(findings[0].candidates.size() == 2);
        assert(findings[0].candidates[0].confidence == Confidence::High);
        assert(findings[0].candidates[0].file.filePath == "/S/Contents/New/B.mp3");
        assert(findings[0].candidates[1].confidence == Confidence::NeedsConfirming);
        assert(findings[1].expectedPath == "/S/Contents/Old/A.mp3");
        assert(findings[1].oneClick());
        std::cout << "case 6 (rows with no file are skipped; order kept; high first) OK\n";
    }

    std::cout << "moved_files_test: all cases OK\n";
    return 0;
}
