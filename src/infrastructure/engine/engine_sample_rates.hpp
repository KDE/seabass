// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"

namespace seabass::infrastructure::engine
{

// A track whose Engine row does not say what its sample rate is.
//
// Engine keeps every cue position as a sample offset, so the sample rate
// is what turns those into times. Without one, everything reading the
// library has to guess -- this project guesses 44.1 kHz, which is right
// for most tracks and wrong by 9% for a 48 kHz one, putting a cue five
// minutes in almost half a minute out of place. The file itself knows,
// which is what makes this worth offering to fix rather than only to
// report.
struct SampleRateEntry
{
    std::int64_t trackId = 0;
    std::string title;
    std::string artist;
    // Absolute, resolved against the library, so the audio file can be
    // asked what the row does not say.
    std::string trackFile;
    // What the file says, when the audit was given something to ask it
    // with: 0 when nothing could be read, and then this entry is a
    // finding without a fix.
    double sampleRateFromFile = 0.0;
};

struct SampleRateAudit
{
    int tracksChecked = 0;
    // Rows the player has not analysed yet: no track data blob at all,
    // which is what Engine OS's own rekordbox import writes. Left out of
    // `missing` (#58 follow-up, 2026-10-04): the rate is one field of the
    // analysis the player writes on first load, Library Health's analysis
    // card already reports these rows, and filling the rate in would mean
    // writing an analysis-shaped blob into a row the player has not
    // analysed. Counted so a page can say why they are not listed.
    int notYetAnalysed = 0;
    std::vector<SampleRateEntry> missing;
    std::string error;  // the library could not be read at all

    // Entries the file could answer for.
    int fixable() const;
};

// What a file says its sample rate is, in Hz, or 0. Passed in because
// reading it means TagLib, which this layer does not link.
using SampleRateProbe = std::function<double(const std::string &audioFile)>;

// `cancel` is checked before every track, and a stop throws
// application::OperationCancelled rather than coming back as an error or
// as a partial count.
// `progress` hears "Checking sample rates" with the library's track count
// as its total and a tick per track: the probe opens one audio file per
// row without a rate, which on a USB stick is minutes for a library that
// lacks many.
SampleRateAudit auditSampleRates(const std::string &engineLibraryPath, const SampleRateProbe &probe = {},
                                 const application::CancellationToken &cancel = application::CancellationToken::none(),
                                 application::ProgressReporter &progress = application::NullProgressReporter::instance());

struct SampleRateRepair
{
    int repaired = 0;
    int skipped = 0;  // the file could not say after all
    std::string error;
};

// Writes each entry's sampleRateFromFile into its Engine row. Entries
// with nothing to write are skipped rather than failing the run: one
// unreadable file is not worth abandoning a thousand rows, the same rule
// the artwork repair follows.
SampleRateRepair repairSampleRates(const std::string &engineLibraryPath,
                                    const std::vector<SampleRateEntry> &entries,
                                    const std::function<void(const std::string &)> &beforeWrite = {},
                                    const std::string &databaseFileOverride = {});

}  // namespace seabass::infrastructure::engine
