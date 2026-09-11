// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "application/ports/cue_writer.hpp"
#include "application/ports/library_cleanup_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"

namespace seabass::gui
{

// application::CueWriter identifies a track by its format's own
// sourceId; infrastructure::onelibrary::OneLibraryCueWriter deliberately
// doesn't (see its own class comment: content_id is a separate id space
// from export.pdb/m.db's own track ids, file path is the one identifier
// every format actually shares). This adapts the latter to the former,
// using a sourceId->filePath map built from the tracks actually being
// written -- see DuplicatesController's own use of this for why no
// track REMOVAL is involved here (this only ever backs the non-
// destructive "copy cues" flow; Clean Up Duplicates' destructive removal
// doesn't support OneLibrary as a primary target yet, since
// OneLibraryCueWriter::removeTrackByPath() deletes playlist-membership
// rows outright rather than reassigning them to a survivor the way
// application::LibraryCleanupWriter::removeTrackReplacingWith()
// requires -- see cleanup_controller.cpp's own comment).
class OneLibraryCueWriterAdapter : public application::CueWriter
{
public:
    // realStickRoot: only needed when pioneerRoot points at a relocated
    // copy of exportLibrary.db (a local scratch build, see
    // OneLibraryCueWriter's own constructor comment) rather than the
    // real stick -- forwarded through unchanged so file-path-to-
    // content.path resolution still uses the real stick's layout.
    OneLibraryCueWriterAdapter(std::string pioneerRoot, std::unordered_map<std::string, std::string> sourceIdToPath,
                                std::optional<std::string> realStickRoot = std::nullopt)
        : m_pioneerRoot(std::move(pioneerRoot)),
          m_sourceIdToPath(std::move(sourceIdToPath)),
          m_realStickRoot(std::move(realStickRoot))
    {
    }

    void writeHotCues(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues) override
    {
        auto it = m_sourceIdToPath.find(trackSourceId);
        if (it == m_sourceIdToPath.end()) {
            throw std::runtime_error("onelibrary: no known file path for source id=" + trackSourceId);
        }
        writer().writeCuesForPath(it->second, cues);
    }

    // For a caller that learns its tracks one at a time rather than
    // handing over the whole map up front -- a per-track staged change,
    // where knowing every path in advance would mean building the map
    // twice. Lets one adapter serve a whole save; see writer() below for
    // why that matters.
    void notePath(const std::string &trackSourceId, std::string filePath)
    {
        m_sourceIdToPath[trackSourceId] = std::move(filePath);
    }

    // The save's one writer for this database, from
    // sharedOneLibraryWriter(). Set this whenever the adapter is built
    // inside a save: a writer refreshes its staleness baseline only
    // after its OWN writes, so two instances against one file make the
    // second throw "changed since this writer was opened" as soon as the
    // first writes -- which is why that helper is keyed on the database
    // and not on the feature. Without it the adapter falls back to a
    // writer of its own, for callers outside a save (tests, and the
    // one-shot paths that build an adapter and use it once).
    void useSharedWriter(infrastructure::onelibrary::OneLibraryCueWriter &shared) { m_shared = &shared; }

    // The writer this adapter writes through, for a caller that needs a
    // OneLibrary write this port does not express -- a rating, a
    // comment. Going through this rather than opening another writer is
    // the difference between one key derivation per save and two, and
    // between a save that works and one that throws on the second write.
    infrastructure::onelibrary::OneLibraryCueWriter &writer()
    {
        if (m_shared) {
            return *m_shared;
        }
        // Built on first use and kept, not built per call. Every open
        // derives the SQLCipher key from a passphrase, ~115 ms of CPU,
        // so a writer constructed per item put that derivation on every
        // item -- the 235 ms/item that round 4 of
        // docs/write-path-performance.md set out to remove, and which it
        // removed from its own nine call sites by holding the writer in
        // SaveContext::shared while this adapter went on rebuilding one
        // underneath.
        if (!m_writer) {
            m_writer = std::make_unique<infrastructure::onelibrary::OneLibraryCueWriter>(m_pioneerRoot,
                                                                                          m_realStickRoot);
        }
        return *m_writer;
    }

private:
    std::string m_pioneerRoot;
    std::unordered_map<std::string, std::string> m_sourceIdToPath;
    std::optional<std::string> m_realStickRoot;
    // Exactly one of these is used: the save's shared writer when a save
    // set one, otherwise this adapter's own.
    infrastructure::onelibrary::OneLibraryCueWriter *m_shared = nullptr;
    std::unique_ptr<infrastructure::onelibrary::OneLibraryCueWriter> m_writer;
};

// Same sourceId->filePath adaptation as OneLibraryCueWriterAdapter above,
// for application::LibraryCleanupWriter instead of CueWriter -- backs
// Clean Up Duplicates when OneLibrary is the primary scanned format.
// Only usable now because OneLibraryCueWriter::
// removeTrackByPathReplacingWith() exists (it didn't when OneLibrary
// support was first added here -- see that method's own comment on the
// real playlist-membership-loss bug its absence caused).
class OneLibraryCleanupWriterAdapter : public application::LibraryCleanupWriter
{
public:
    // realStickRoot: see OneLibraryCueWriterAdapter's own comment above.
    OneLibraryCleanupWriterAdapter(std::string pioneerRoot,
                                    std::unordered_map<std::string, std::string> sourceIdToPath,
                                    std::optional<std::string> realStickRoot = std::nullopt)
        : m_pioneerRoot(std::move(pioneerRoot)),
          m_sourceIdToPath(std::move(sourceIdToPath)),
          m_realStickRoot(std::move(realStickRoot))
    {
    }

    void removeTrackReplacingWith(const std::string &doomedTrackId, const std::string &survivorTrackId) override
    {
        auto doomedIt = m_sourceIdToPath.find(doomedTrackId);
        if (doomedIt == m_sourceIdToPath.end()) {
            throw std::runtime_error("onelibrary: no known file path for source id=" + doomedTrackId);
        }
        auto survivorIt = m_sourceIdToPath.find(survivorTrackId);
        if (survivorIt == m_sourceIdToPath.end()) {
            throw std::runtime_error("onelibrary: no known file path for source id=" + survivorTrackId);
        }
        infrastructure::onelibrary::OneLibraryCueWriter writer(m_pioneerRoot, m_realStickRoot);
        writer.removeTrackByPathReplacingWith(doomedIt->second, survivorIt->second);
    }

private:
    std::string m_pioneerRoot;
    std::unordered_map<std::string, std::string> m_sourceIdToPath;
    std::optional<std::string> m_realStickRoot;
};

}  // namespace seabass::gui
