// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QEventLoop>
#include <QObject>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>

// A QAudioDecoder that starts on a sound file and then says nothing at
// all, and what every user of one here does about it.
//
// The fault is Qt Multimedia's (FFmpeg backend, seen with Qt 6.11 and
// FFmpeg 6.1; ticket 154): its renderer's thread copies the codec
// context's channel layout while FFmpeg's decoding threads rewrite it,
// now and then catches it empty, and cannot set up its resampler. FFmpeg
// logs "Input channel layout "" is invalid or unsupported", and then no
// buffer comes, no end and no error. Measured on FLAC, through the PCM
// source: one start in 4,500 while whole tracks were analysed eight at
// a time, and 20 in 288,000 starts that stopped at the first buffer,
// twelve at a time; MP3 too, more rarely. In these runs not the same
// file twice: the file is sound and the next start works (all 20
// opened at the second attempt). Asking the backend for no format
// changes nothing (23 in 288,000 of the same starts): it builds the
// resampler either way.
//
// So a decode is started up to three times. Only a start that gave
// NOTHING before its first sample is tried again: no buffer, no end, no
// error. A file the decoder refuses is refused at once, and whatever
// happens after the first buffer is the caller's own business, as it
// was (samples already handed on cannot be taken back). The attempts
// before the last wait FirstSampleWaitMs for that first sign (or the
// caller's timeout, where that is shorter); the last one has no limit
// of its own, only the caller's timeout, so a device that is slow to
// wake keeps the time it had.
//
// Not cured by this: with a newer FFmpeg the same race ends in a crash
// inside libswresample. That needs Qt fixed, or a decoder of our own.
namespace seabass::infrastructure::audio::decoder_start
{

inline constexpr int Attempts = 3;
inline constexpr int FirstSampleWaitMs = 3000;

struct Attempt
{
    // 1 to Attempts.
    int number = 1;
    // How long to wait for the decoder's first sign; 0 on the last
    // attempt, which has the caller's own timeout and no other.
    int firstSampleWaitMs = 0;
    // A test's: do not start the decoder, which is what the fault looks
    // like from outside.
    bool silenced = false;
};

namespace detail
{
inline std::atomic<int> silentAttempts{0};
inline std::atomic<int> silentAttemptWaitMs{0};
}  // namespace detail

// For tests. The first `attempts` attempts of every decode started from
// now on give nothing (0 ends it), and such an attempt, where another
// follows it, is waited for `waitMs` in place of FirstSampleWaitMs (0:
// as shipped). Attempts that do decode keep their times.
inline void setSilentAttemptsForTesting(int attempts, int waitMs = 0)
{
    detail::silentAttempts = attempts;
    detail::silentAttemptWaitMs = waitMs;
}

// Runs `body(attempt)` until it says the decoder gave something (false)
// or the attempts are used up; `body` returns true for an attempt that
// gave nothing before its first sample. Returns how many attempts were
// made. Each further attempt is a line on standard error, so the rate
// stays seen.
template <typename Body>
int run(const std::string &file, Body &&body)
{
    for (int number = 1; number <= Attempts; ++number) {
        Attempt attempt;
        attempt.number = number;
        attempt.silenced = number <= detail::silentAttempts;
        if (number < Attempts) {
            attempt.firstSampleWaitMs =
                attempt.silenced && detail::silentAttemptWaitMs > 0 ? int(detail::silentAttemptWaitMs) : FirstSampleWaitMs;
        }
        if (!body(attempt) || number == Attempts) {
            return number;
        }
        std::cerr << "warning: audio decoder: no sign of life on attempt " << number << " of " << Attempts
                  << ", trying again: " << file << "\n";
    }
    return Attempts;
}

// One attempt's watch, for a decode that runs in an event loop of its
// own: heard() at every sign of the decoder (a buffer, the end, an
// error); where none came within the attempt's wait, the loop is quit
// and gaveNothing() is true. `timeoutMs` is the caller's for the whole
// decode: the wait is never longer than that.
//
// heard() answers whether the sign still counts. Once the watch has
// given the attempt up it does not: a buffer that arrives in the very
// turn of the loop in which the wait ran out belongs to an attempt
// that is over, and whoever takes it would hand on the start of the
// file twice. So every handler begins `if (!watch.heard()) return;`.
class FirstSampleWatch
{
public:
    FirstSampleWatch(const Attempt &attempt, QEventLoop &loop, int timeoutMs)
    {
        if (attempt.firstSampleWaitMs <= 0) {
            return;
        }
        m_timer.setSingleShot(true);
        QObject::connect(&m_timer, &QTimer::timeout, &loop, [this, &loop]() {
            if (!m_heard) {
                m_gaveNothing = true;
                loop.quit();
            }
        });
        m_timer.start(timeoutMs > 0 ? std::min(attempt.firstSampleWaitMs, timeoutMs) : attempt.firstSampleWaitMs);
    }

    bool heard()
    {
        if (m_gaveNothing) {
            return false;
        }
        m_heard = true;
        return true;
    }
    bool gaveNothing() const { return m_gaveNothing; }

private:
    QTimer m_timer;
    bool m_heard = false;
    bool m_gaveNothing = false;
};

}  // namespace seabass::infrastructure::audio::decoder_start
