// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

#include "application/ports/cancellation_token.hpp"

namespace seabass::gui
{

// What this computer has for analysing several tracks at once: its cores
// by strength, and the memory that is free right now.

// A computer's cores, counted as cores and not as threads. Where all
// cores are alike (or the system does not say), they are all
// `performance`.
struct CpuCores
{
    int performance = 0;
    int efficiency = 0;
};

// Asked of the system: macOS's performance levels, Linux's cpu_core and
// cpu_atom lists (and its core ids, so that two threads of one core are
// one core), Windows' efficiency classes. {0, 0} when nothing answers.
CpuCores cpuCores();

// How many tracks to analyse at once on such a computer: one for each
// performance core and one for every two efficiency cores, two at least
// and eight at most. The workers take tracks from one queue, so a worker
// on a weaker core takes fewer and holds nobody up. `threads` is what is
// left to go by when the cores are not known: half of them.
int analysisJobsFor(CpuCores cores, int threads);

// Has the calling thread treated as work the user is waiting for, where
// the system tells its cores apart by that (macOS puts such threads on
// its performance cores). Nothing elsewhere.
void markAnalysisThread();

// Memory a new allocation could have now without the system swapping:
// MemAvailable on Linux, free, inactive and purgeable pages on macOS,
// available physical memory on Windows. nullopt when it cannot be read.
std::optional<std::uint64_t> availableMemoryBytes();

// What a track takes at its peak while it is analysed whole, measured:
// 2.8 GB for a mix of 65 minutes, 250 MB for six minutes.
constexpr std::uint64_t AnalysisBytesPerSecond = 720ull * 1024;
// What a track not yet read is taken to need: eight minutes' worth.
constexpr std::uint64_t AnalysisTypicalBytes = 8 * 60 * AnalysisBytesPerSecond;
// Left to the rest of the computer.
constexpr std::uint64_t AnalysisMemoryMargin = 1024ull * 1024 * 1024;
// Assumed free when the system does not say.
constexpr std::uint64_t AnalysisMemoryWhenUnknown = 4ull * 1024 * 1024 * 1024;

// Whether another track may start: always when none is in hand (an import
// never stands still, a track too large for the computer is slow, not
// refused), otherwise when what is free now, less what the tracks in hand
// are still going to take, has room for a typical track and the margin.
bool analysisMayStart(std::optional<std::uint64_t> availableBytes, std::uint64_t stillToTakeBytes, int inHand);

// Asked before each track is started, not once for an import: what else
// the computer does during a long import is seen at the next track.
//
// The reading of free memory lags: a track just started has not taken
// its memory yet. So each track in hand says how far it has got (noteRead)
// and the gate takes off what it will still take: its peak for what was
// read so far (a typical track's at least, its length is not known before
// it is read), less what it holds already.
class AnalysisMemoryGate
{
public:
    using Reader = std::function<std::optional<std::uint64_t>()>;
    explicit AnalysisMemoryGate(Reader available = availableMemoryBytes);

    // Blocks until a track may start; asks `cancel` while it waits and
    // throws application::OperationCancelled. Returns the track's ticket.
    int enter(const application::CancellationToken &cancel);
    // The track of `ticket` has read `seconds` of audio and holds
    // `heldBytes` for it.
    void noteRead(int ticket, double seconds, std::uint64_t heldBytes);
    void leave(int ticket);

    int inHand() const;
    std::uint64_t stillToTake() const;

    // The gate the app's analysis goes through.
    static AnalysisMemoryGate &shared();

private:
    struct Track
    {
        std::uint64_t peakBytes = AnalysisTypicalBytes;
        std::uint64_t heldBytes = 0;
    };
    std::uint64_t stillToTakeLocked() const;

    Reader m_available;
    mutable std::mutex m_mutex;
    std::condition_variable m_left;
    std::map<int, Track> m_tracks;
    int m_nextTicket = 1;
};

}  // namespace seabass::gui
