// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QString>

#include <string>

class QMediaPlayer;

namespace seabass::infrastructure::audio
{

// #109: Qt Multimedia's FFmpeg player keeps one timeline for an MP3 from
// its start and another after a seek. FFmpeg trims a LAME or Lavc file's
// encoder delay (1105 samples for the usual 576) and, with no packet time
// base set by Qt, leaves the frame the trim ends in at the time of its
// packet: 47 samples stamped 1105 samples (25.06 ms at 44.1 kHz) early.
// Qt's player counts the time of every later sample from the first frame
// it renders, so a file played from 0 plays each sound 25 ms (23 at 48
// kHz, 24 for MPEG-2) before the position it reports for it, until a seek
// starts the count again from a frame of its own. A seek that lands
// past that one cut frame drops it (Qt drops the frames that end before
// the seek), and from then on the player's timeline is the decoder's,
// the one playback_timeline.hpp and its cue and beat shifts are on.
// Measured with tests/player_start_timeline_test.cpp on the click files.
//
// The earliest position, in ms, the player can be at on this file and
// keep that timeline: just past the cut frame, its samples * 1000 / rate
// + 1 (domain::ffmpegPartialFirstFrameSamples()). For the usual 576-sample
// delay that is 47 samples: 2 ms at 44.1 kHz, 1 at 48, 3 at 22.05, 6 at
// 8 kHz; a tag stating another delay can leave up to a frame less one
// sample (26 ms at 44.1 kHz, 72 at 8 kHz). 0 for a file with no cut frame
// (not an MP3, nothing trimmed, a lead-in that cannot be read). Measured
// tight where the cut frame is the stream's first (a usual 44.1 kHz file
// is still off after a seek to 1 ms, right from 2); where the trim runs
// past the first frame (MPEG-2, a 1200-sample delay) any seek, even to 0,
// was enough, so there this skips more than it has to, never less.
qint64 playerStartMs(const std::string &filePath);

// Keeps a QMediaPlayer at or past playerStartMs() of the file it plays:
// on loading (and after stop(), which leaves the player loaded at 0), on
// every seek, and on a play() that starts the file over from its end.
// What the player skips is that start, the cut frame (2 ms for a usual
// 44.1 kHz file), where playing from 0 lost 27 ms of it (the cut frame
// and the next, which Qt stamped before 0).
class PlayerStartGuard : public QObject
{
public:
    explicit PlayerStartGuard(QMediaPlayer *player, QObject *parent = nullptr);

    // The file about to be given to the player; empty for none. Call it
    // before setSource(), so the load is met with the file's start.
    void setFile(const QString &filePath);
    qint64 startMs() const { return m_startMs; }

    // setPosition(), never before startMs(). Returns the position set.
    qint64 seek(qint64 positionMs);
    // play(), from startMs() where the player would start from 0: at its
    // end, stopped, or before the file has loaded.
    void play();

private:
    QMediaPlayer *m_player;
    qint64 m_startMs = 0;
};

}  // namespace seabass::infrastructure::audio
