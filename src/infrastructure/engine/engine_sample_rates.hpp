// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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
    std::vector<SampleRateEntry> missing;
    std::string error;  // the library could not be read at all

    // Entries the file could answer for.
    int fixable() const;
};

// What a file says its sample rate is, in Hz, or 0. Passed in because
// reading it means TagLib, which this layer does not link.
using SampleRateProbe = std::function<double(const std::string &audioFile)>;

SampleRateAudit auditSampleRates(const std::string &engineLibraryPath, const SampleRateProbe &probe = {});

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
