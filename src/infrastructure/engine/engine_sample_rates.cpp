// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/engine_sample_rates.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>

#include <djinterop/djinterop.hpp>

namespace seabass::infrastructure::engine
{

namespace fs = std::filesystem;

namespace
{

fs::path databaseDirectory(const std::string &engineLibraryPath)
{
    return pathFromUtf8(engineLibraryPath) / "Database2";
}

// A row's own sample rate, or nothing. Reading it can throw rather than
// return nothing: a track whose data blob is the wrong length (which is
// how this turns up on a real stick, in the log as "sample_rate
// unreadable") fails inside libdjinterop, and a library full of those
// must still be audited to the end.
// `unreadable` is set when the record could not be decoded, as opposed to
// decoding to no rate. Only the second is a finding: a record Seabass
// cannot decode is one it does not understand -- seven "unreadable" rows on
// a stick a Prime 4 had played turned out to be healthy Engine 3.x records
// with 24 extra bytes -- and nothing is said about what is not understood.
std::optional<double> rateOf(djinterop::track &track, bool &unreadable)
{
    unreadable = false;
    try {
        const auto rate = track.sample_rate();
        if (rate && *rate > 0.0) {
            return rate;
        }
    } catch (const std::exception &) {
        unreadable = true;
    }
    return std::nullopt;
}

std::string textOf(const std::optional<std::string> &value)
{
    return value ? *value : std::string();
}

}  // namespace

int SampleRateAudit::fixable() const
{
    return static_cast<int>(
        std::count_if(missing.begin(), missing.end(), [](const SampleRateEntry &e) { return e.sampleRateFromFile > 0.0; }));
}

SampleRateAudit auditSampleRates(const std::string &engineLibraryPath, const SampleRateProbe &probe,
                                 const application::CancellationToken &cancel)
{
    SampleRateAudit audit;
    std::error_code ec;
    if (!fs::is_directory(databaseDirectory(engineLibraryPath), ec)) {
        return audit;  // no Engine library here: not a fault, nothing to say
    }
    try {
        // The library directory, not Database2: libdjinterop appends
        // that itself, and handed the inner path it opens something that
        // answers every read with "SQL logic error".
        auto db = djinterop::engine::load_database(engineLibraryPath);
        for (djinterop::track track : db.tracks()) {
            cancel.throwIfCancelled();
            audit.tracksChecked++;
            bool unreadable = false;
            if (rateOf(track, unreadable) || unreadable) {
                continue;
            }
            SampleRateEntry entry;
            entry.trackId = track.id();
            try {
                entry.title = textOf(track.title());
                entry.artist = textOf(track.artist());
                // Engine stores it relative to the library directory.
                const fs::path relative = pathFromUtf8(track.relative_path());
                entry.trackFile = pathToUtf8(fs::weakly_canonical(pathFromUtf8(engineLibraryPath) / relative, ec));
                if (ec) {
                    entry.trackFile.clear();
                }
            } catch (const std::exception &) {
                // A row too damaged to describe is still a row missing a
                // sample rate; it just cannot be fixed from its file.
            }
            if (probe && !entry.trackFile.empty() && fs::is_regular_file(pathFromUtf8(entry.trackFile), ec)) {
                entry.sampleRateFromFile = probe(entry.trackFile);
            }
            audit.missing.push_back(std::move(entry));
        }
    } catch (const application::OperationCancelled &) {
        throw;  // a stop, not an unreadable library
    } catch (const std::exception &e) {
        audit.error = std::string("could not read the Engine library: ") + e.what();
    }
    // Fixable first, so a page can offer them without sorting.
    std::stable_partition(audit.missing.begin(), audit.missing.end(),
                          [](const SampleRateEntry &e) { return e.sampleRateFromFile > 0.0; });
    return audit;
}

SampleRateRepair repairSampleRates(const std::string &engineLibraryPath,
                                    const std::vector<SampleRateEntry> &entries,
                                    const std::function<void(const std::string &)> &beforeWrite,
                                    const std::string &databaseFileOverride)
{
    SampleRateRepair result;
    const fs::path database = databaseFileOverride.empty()
        ? databaseDirectory(engineLibraryPath) / "m.db"
        : pathFromUtf8(databaseFileOverride);
    try {
        if (beforeWrite) {
            beforeWrite(pathToUtf8(database));
        }
        // Same again: from <root>/Database2/m.db back up to <root>.
        auto db = djinterop::engine::load_database(pathToUtf8(database.parent_path().parent_path()));
        for (const SampleRateEntry &entry : entries) {
            if (entry.sampleRateFromFile <= 0.0) {
                result.skipped++;
                continue;
            }
            // Per row, because the rows this function is given are by
            // definition the damaged ones: a track whose data blob is
            // the wrong length throws inside libdjinterop, and one of
            // those must not take the other nine hundred with it.
            try {
                auto track = db.track_by_id(entry.trackId);
                if (!track) {
                    result.skipped++;  // gone since the audit; not this run's business
                    continue;
                }
                track->set_sample_rate(entry.sampleRateFromFile);
                result.repaired++;
            } catch (const std::exception &) {
                result.skipped++;
            }
        }
    } catch (const std::exception &e) {
        // Opening the library failed, so nothing was written: this is the
        // one case where the count really is zero.
        result.error = std::string("could not write the sample rates: ") + e.what();
        result.repaired = 0;
    }
    return result;
}

}  // namespace seabass::infrastructure::engine
