<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC0-1.0
-->

# MP3 seek pre-roll probe (#92)

A hot cue on a transient in an MP3 is silent in the in-app player: after a
seek, FFmpeg's MP3 decoder has none of the bit reservoir the next frames
borrow from and delivers silence until it has. The workaround the issue
proposes is a pre-roll: seek about 200 ms before the cue with the output
muted, unmute at the cue. These tools measure both halves of that:
whether a pre-roll gets the transient decoded (it does), and how close to
the cue the unmute can be placed on the player's audio path (not close:
see below). No fix is in the player; this is what was found instead.

## What each tool answers

| tool | question | needs |
|---|---|---|
| `seek_probe` | after a seek, where does the first sound come out of the decoder? | no sound card |
| `sink_probe` | where does an unmute (modes `tap`, `clock`) land in what the sound server plays? | a null sink |
| `measure-unmute.sh` | makes the null sink, records it, runs `sink_probe` and `analyse_unmute.py` | PipeWire with its Pulse layer |
| `analyse_unmute.py` | reads the recordings | Python 3 |
| `make-probe-audio.py` | writes the probe audio | ffmpeg with libmp3lame |

## Build and run

The probes are not part of the CMake build (they need a sound server or
a null sink, which neither the suite nor CI has). Build them by hand into
a scratch directory, outside the source tree:

    src=$PWD/tools/mp3-seek-preroll-probe
    mkdir -p ~/tmp/mp3-probe && cd ~/tmp/mp3-probe
    for t in seek_probe sink_probe; do
        g++ -std=c++17 -fPIC -O1 "$src/$t.cpp" -o "$t" $(pkg-config --cflags --libs Qt6Multimedia Qt6Core)
    done
    "$src/make-probe-audio.py" .

Then:

    QT_QPA_PLATFORM=offscreen ./seek_probe toneclick.mp3 1450
    LC_ALL=C "$src/measure-unmute.sh" . toneclick.mp3 1500 40 400 plain tap clock
    LC_ALL=C LEAD=22 "$src/measure-unmute.sh" . toneclick.mp3 1500 40 400 clock

Nothing here plays on a real output. `measure-unmute.sh` loads a PipeWire
null sink (`seabass_probe`) for the run, puts the default output back if
the new sink took it, records the sink's monitor with `pw-record` and
unloads the sink when it exits. `sink_probe` takes the device to play on
as an argument and refuses to start when no output device matches it.
`LC_ALL=C` keeps the probes' decimal points (QCoreApplication sets the
locale's).

## 1. What the decoder delivers after a seek (`seek_probe`)

Qt 6.11.1, FFmpeg backend, Linux. Seek, play, first sample above 0.05 on
the player's clock. Every file has a click at 1.500 s (the class A fixture
plays it 25.06 ms late: it has no tag, so FFmpeg trims no encoder delay).

| file | click | seek → first sound |
|---|---|---|
| click-A-notag-44100.mp3 (64k) | 1524.94 | 1500, 1480, 1450 → **2525.03 (missed)**; 1420 → 1524.94 |
| click-D-lame-44100.mp3 (64k) | 1499.89 | 1500 … 1420 → **2499.98 (missed)**; 1400 and earlier → 1499.89 |
| click-D-lame-48000.mp3 (80k) | 1499.92 | 1500 … 1450 → **missed**; 1420 → 1499.85; 1400 and earlier → 1499.92 |
| click-D-lame-mpeg2-22050.mp3 | 1499.77 | 1500 … 1450 → **missed**; 1420 and earlier → 1499.77 |
| click.mp3 (LAME 192k) | 1499.93 | 1500 → **missed**; 1490 → 1500.07 (partly); 1450 and earlier → 1499.93 |
| click.wav | 1500.00 | every seek: 1500.00; the stream starts at the seek, to the sample |
| click.flac | 1500.00 | every seek, 1500 to 1300: 1500.00 |
| click.m4a (AAC 192k) | 1499.95 | every seek, 1500 to 1300: 1499.95 |

So the loss is MP3 only and depends on the bit rate (the reservoir is a
byte count): up to 80 to 90 ms after the frame the stream restarts on at
64 kbit/s, under 40 ms at 192 kbit/s. A seek 200 ms before the cue
delivers the click on the sample in every file. FLAC and AAC lose
nothing.

## 2. Where an unmute lands in what is heard (`sink_probe`)

`toneclick.mp3`: a 440 Hz tone at 0.15 under clicks at 1.5, 2.5, 3.5 s, so
any sound before the cue shows as tone in the recording. Cue at 1500,
jumps 400 ms apart, recorded from a 48 kHz null sink. "Heard before the
cue" is how much tone came out before the click.

| mode | jumps | click not heard | heard before the cue (min / median / max) |
|---|---|---|---|
| plain (today: seek to the cue) | 40 / 15 | 40 / 15 | none heard |
| tap (unmute on the buffer that holds the cue) | 40 / 40 | 0 / 0 | 47.6 / 69.0 / 90.3 (47.6 / 47.6 / 69.0) |
| clock, unmute when position() reaches the cue | 40 / 40 | 13 / 27 | 5.0 / 5.0 / 5.0 (5.0 / 5.0 / 47.6) |
| clock, 10 ms early | 40 / 41 | 0 / 1 | 26.3 / 47.6 / 69.0 (26.3 / 47.6 / 69.0) |
| clock, 22 ms early | 40 / 40 | 0 / 0 | 26.3 / 47.6 / 90.3 (26.3 / 47.6 / 90.3) |

Two runs each, 2026-11-19 on a Framework laptop (PipeWire 1.6.0): the
first numbers with three other builds running (load 12), the second, in
parentheses, earlier with the machine quieter.

Every value is 5.0 ms (for this cue and seek point) plus a multiple of
21.33 ms: 1024 frames at 48 kHz,
the PipeWire quantum. That is the finest an unmute can be placed, and it
cannot be finer on this path:

- The player plays through QAudioOutput. The only control it has over
  what is heard is the output's volume and mute. Qt's FFmpeg renderer
  hands a volume change to its QAudioSink, which stores it and applies
  it in the sound server's callback to the whole period it fills
  (qpipewire_audiosink.cpp, qpulseaudiosink.cpp: runAudioCallback(...,
  volume())). Mute takes effect on a period boundary, never on a sample.
- The tap (QAudioBufferOutput) gets each frame after the renderer has
  already written it to the sink (AudioRenderer::renderInternal pushes to
  the sink first). Its timestamps say what was decoded, not when it is
  heard: unmuting on them comes the sink's fill level early, 47.6 ms
  here, more when the renderer lets the fill drift up (69 ms).
- position() is Qt's estimate of what is heard. Unmuting on it lands on
  the next period boundary, which cuts the transient whenever the cue is
  not in the first few ms of a period: most jumps.

Any lead that keeps the transient whole lets 26 to 90 ms of what comes
before the cue through, with a hard step from silence to full volume at
the start of it. The period is the sound server's and is not known to
the player (21.33 ms here; PipeWire changes it with the other clients
running; WASAPI and Core Audio have their own).

A sample-exact gate needs the player to write the samples to the sink
itself (QMediaPlayer with only a QAudioBufferOutput, the samples gated
and written to a QAudioSink of Seabass's own). The player has no such
path today.
