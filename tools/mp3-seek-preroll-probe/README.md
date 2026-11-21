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
see below). Sections 3 to 8 hold the cheaper alternatives looked at on
2026-11-21. No fix is in the player; this is what was found instead.

## What each tool answers

| tool | question | needs |
|---|---|---|
| `seek_probe` | after a seek, where does the first sound come out of the decoder? (`PACE=1`: how does a player with only a QAudioBufferOutput pace its buffers?) | no sound card |
| `sink_probe` | where does an unmute (modes `tap`, `clock`) or a resume from a muted hold at the cue (`warm`, `warmtap`) land in what the sound server plays? | a null sink |
| `handover_probe` | can the cue's first milliseconds come from a buffer decoded ahead with QAudioDecoder, played on a QAudioSink of our own, with the player taking over after them? | a null sink |
| `warm_cost` | what do extra QMediaPlayers, each held muted at a hot cue, cost? | a null sink |
| `measure-unmute.sh` | makes the null sink, records it, runs `sink_probe` or `handover_probe` and the matching analysis | PipeWire with its Pulse layer |
| `analyse_unmute.py`, `analyse_handover.py` | read the recordings | Python 3 |
| `make-probe-audio.py` | writes the probe audio | ffmpeg with libmp3lame |

## Build and run

The probes are not part of the CMake build (they need a sound server or
a null sink, which neither the suite nor CI has). Build them by hand into
a scratch directory, outside the source tree:

    src=$PWD/tools/mp3-seek-preroll-probe
    mkdir -p ~/tmp/mp3-probe && cd ~/tmp/mp3-probe
    for t in seek_probe sink_probe handover_probe warm_cost; do
        g++ -std=c++17 -fPIC -O1 "$src/$t.cpp" -o "$t" $(pkg-config --cflags --libs Qt6Multimedia Qt6Core)
    done
    "$src/make-probe-audio.py" .

`handover_probe` needs Qt 6.11 (QAudioSink's callback API); the others
build with 6.8 and later. Then:

    QT_QPA_PLATFORM=offscreen ./seek_probe toneclick.mp3 1450
    LC_ALL=C "$src/measure-unmute.sh" . toneclick.mp3 1500 40 400 plain tap clock
    LC_ALL=C LEAD=22 "$src/measure-unmute.sh" . toneclick.mp3 1500 40 400 clock
    LC_ALL=C "$src/measure-unmute.sh" . toneclick.mp3 1500 40 400 warmtap handover

Nothing here plays on a real output. `measure-unmute.sh` loads a PipeWire
null sink (`seabass_probe`) for the run, puts the default output back if
the new sink took it, records the sink's monitor with `pw-record` and
unloads the sink when it exits. `sink_probe`, `handover_probe` and
`warm_cost` take the device to play on as an argument and refuse to start
when no output device matches it; give them `seabass_probe` and run
`warm_cost` while such a sink exists. `LC_ALL=C` keeps the probes' decimal
points (QCoreApplication sets the locale's).

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

## 3. A buffer decoded ahead, handed over to the player (`handover_probe`)

Measured 2026-11-21, same machine, Qt 6.11.1, PipeWire 1.6.0, 48 kHz null
sink, 40 jumps per run, cue on the click at 1500. The whole file is
decoded with QAudioDecoder (from its start, so the reservoir is intact);
on each jump the player seeks to the cue and plays, unmuted, while the
probe's own QAudioSink (callback API, same device) plays the decoded
audio from the cue and stops, with a 2 ms fade, where the player's tap
says its own audio begins. "Offset" is how much later the player's
stream sounds than the probe's for the same track time; "gap" is the
silence between the two at the splice.

| file, DELAY | splice (player's audio from) | offset: jumps | gap: jumps |
|---|---|---|---|
| toneclick.mp3 (192k), 0 | cue + 23.2 ms (tap told 5 to 7 ms after the jump) | 31.3: 33, 10.0: 3, 52.6: 1; 3 more jumps broken up by a dropout over 100 ms | 39: 33, 18: 3, 60: 1 |
| toneclick.mp3, 10 | cue + 23.2 ms | 10.0: 23, 31.3: 14, the player's stream skipped 33 to 54 ms: 3 | 18: 23, 39: 16, 60: 1 |
| toneclick-64k.mp3, 0 | cue + 75.4 ms (tap told 56 to 59 ms after the jump) | 31.3: 27, 10.0: 10, 52.6: 1, skipped 118: 1; 1 dropout over 100 ms | 39: 27, 18: 11, 61: 1 |

The click was heard on the cue in every measured jump, and nothing
before it, except two jumps in the DELAY 0 runs where 21.3 ms of sound
came first (the player's stale audio, see 4). But the hand-over is a
dropout every time: 18 ms at best, mostly 39 ms, sometimes 60 or more.
The player's stream starts 10, 31 or 52 ms after the probe's: about
10 ms plus a whole number of 21.33 ms periods, varying from jump to
jump. Delaying the own sink by 10 ms moves the distribution by one
period and does not narrow it, so no fixed delay lines the two up.
Overlapping them instead (letting the own buffer run on and fading it
out under the player) would play the same audio twice 10 to 50 ms apart
(a flam) instead of a gap. Two streams on two sinks cannot be spliced to
within a few ms through Qt's public API; and the player's position()
during the hand-over is the player's (its offset is not known either).

## 4. A decoder held warm at the cue (`sink_probe` modes `warm`, `warmtap`)

Mute, seek 200 ms before the cue, play, pause before the cue, wait
300 ms, unmute, play on: no seek at the jump, so the reservoir is
intact. Same machine and date, 40 jumps each, cue on the click at 1500.

| how the hold is placed | click not heard | heard before the cue |
|---|---|---|
| warm: pause when position() reaches the cue (LEAD 0, 22, 50) | 40 of 40 each | |
| warm, LEAD 80 | 0 of 40 | 36.1 ms every time |
| warm, LEAD 110 | 0 of 40 | 36.1 ms every time |
| warmtap: pause when the tap delivers the buffer before the cue's (toneclick.mp3) | 0 of 40 | 10.0 ms every time |
| warmtap, toneclick-64k.mp3 | 0 of 40 | 10.0 ms every time |

Qt's pause stops the renderer feeding the sink; the sink plays out what
it already holds (muted, so a pause placed less than about 80 ms ahead
of the cue loses the click with it) and play() goes on with the next whole decoded frame.
What is heard after a resume therefore starts on an MP3 frame boundary,
not on a period: 10.0 ms here is where the cue sits in its frame (1490.0
to 1516.2), 36.1 ms is one frame earlier. That is the best this path
can do: up to one frame (1152 samples, 26.1 ms at 44.1 kHz, 24 ms at
48 kHz) of what comes before the cue, decided by the file and the cue,
not by timing, with the transient whole every time. It still lets
earlier audio through.

In 5 of 40 jumps of the `warm` LEAD 80 run and of the `warmtap` 64k run,
8 of the LEAD 22 run and 4 of the LEAD 50 run, 13 ms of tone sounded at
the start of the muted pre-roll (an extra jump in the counts above): the mute set just before setPosition() and play()
does not always reach the renderer before the first audio does.

## 5. What holding players warm costs (`warm_cost`)

Extra QMediaPlayers on a 6 minute 320 kbit/s MP3, each with its own
QAudioOutput on the null sink, each prepared muted and paused at a cue.
The app's own player loaded alongside: 116 MB resident, 5 threads.

| warm players | resident memory added | threads added | ready after |
|---|---|---|---|
| 1 | 2.5 MB | 5 | 251 ms |
| 4 | 7.0 MB | 23 | 253 ms |
| 8 | 13.1 MB | 47 | 253 ms |

Plus one open output stream per warm player (a PipeWire node here, a
WASAPI client or an AudioUnit elsewhere), each held idle for as long as
the track is loaded. A cue that is jumped to is used up: its player
becomes the playing one and the cue needs another 250 ms to be warm
again, so a second press within that time falls back to a seek.

## 6. What decoding ahead costs (QAudioDecoder)

QAudioDecoder has no seek in Qt 6.11: a cue's neighbourhood can only be
had by decoding from the start of the file. A whole 6 minute MP3 (a
sine, 320k and 128k) decoded and converted to 48 kHz float stereo in
295 to 301 ms on this laptop (16 threads, load under 1). Kept whole that
is 138 MB; kept as one second around each of eight hot cues, 3 MB.

## 7. What Qt 6.11.1 lets Seabass change (read in its source, v6.11.1)

- The seek. `Demuxer::ensureSeeked()` (playbackengine/qffmpegdemuxer.cpp)
  calls `av_seek_frame(ctx, -1, pos, AVSEEK_FLAG_BACKWARD)`, the decoder
  is flushed, and the renderer drops only frames that end before the seek
  position. FFmpeg's mp3 demuxer (libavformat/mp3dec.c, read in 8.1.1;
  the Qt here runs on the system's FFmpeg 6.1, libavformat 60) seeks by its TOC only with `usetoc` or
  `AVFMT_FLAG_FAST_SEEK`, otherwise by the generic index; neither
  pre-rolls, and nothing in libavformat or libavcodec decodes the frames
  before the seek point to refill the reservoir. The only options Qt hands
  to `avformat_open_input` are timeout, probesize, protocol whitelist
  (`QT_FFMPEG_PROTOCOL_WHITELIST`) and low-latency flags, set from
  QPlaybackOptions; no environment variable or QMediaPlayer property
  reaches the demuxer's or decoder's options. Without patching Qt there
  is no seek that keeps the reservoir.
- A volume ramp. There is none. QAudioSink::setVolume() stores one
  number; every backend applies it to the whole buffer of the next
  callback (`QAudioHelperInternal::applyVolume`, qpipewire_audiosink.cpp
  `runAudioCallback(..., volume())`), and QAudioOutput forwards its volume
  and mute to the player's internal QAudioSink the same way. Nothing takes
  a sample position.
- An own sink. QAudioSink gained a callback API in 6.11
  (`start(callback)` with a `QSpan<SampleType>`, on a real-time audio
  thread; CoreAudio, Windows, PulseAudio, PipeWire and Android). A
  QAudioBufferOutput given a format makes the player convert and resample
  every buffer to it (`AudioRenderer::updateOutputs`, a swresample
  resampler), so a sink of Seabass's own can be fed in the device's format.
  A player with a QAudioBufferOutput and no QAudioOutput renders each
  frame when the steady clock reaches its time (`Renderer::nextTimePoint`)
  and never adjusts to a sink: the buffers arrive in real time, on the
  system clock, not on the sound card's.

## 8. How a player with only a QAudioBufferOutput paces its buffers (`seek_probe`, `PACE=1`)

toneclick.mp3, seek to 1300, no audio output, 2026-11-21:

| playback rate | buffer for track time t after the seek arrives at |
|---|---|
| 1 | t ms after play(), every 26.1 ms (one MP3 frame each) |
| 4 | t / 4 ms after play(): 320 ms of track in 81 ms |

The buffers are the same at both rates (same timestamps, lengths and first
sound at 1319.4 ms): the rate only drives the renderer's clock, and the
buffer output's resampler does not stretch them. So a sink of Seabass's
own gets the audio just in time on the system clock, and a pre-roll
played through it costs its own length in real time unless the player
runs faster while the pre-roll comes through. Not measured: whether
setting the rate back to 1 at the cue is seamless.
