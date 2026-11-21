<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# MP3 seek pre-roll probe (#92)

A hot cue on a transient in an MP3 is silent in the in-app player: after a
seek, FFmpeg's MP3 decoder has none of the bit reservoir the next frames
borrow from and delivers silence until it has. These tools measure what
can be done about it with the player Seabass has (QMediaPlayer, Qt's
FFmpeg backend). No fix is in the player; this is what was found.

**Result, 2026-11-21.** Decoding the MP3 once, when the track is loaded,
into a 16-bit WAV and giving that WAV to the unchanged QMediaPlayer makes
jumps land on the cue with the transient whole and nothing from before
it: on both click files every jump starts on the click (a few in a
hundred start with one leftover period of the previous position, as
they do today with any source), against every click lost on the MP3; on
a real 6-minute track 40 of 40 start on the cue, against 39 of 40 that
lose the first 11 ms on the MP3 (the 40th matched too poorly to say)
(section 6). The same holds for a jump made while playing. The WAV
plays the same samples at the same position as the MP3 after a seek.
Decoding takes about 1 ms of wall time per second of audio on this
laptop (0.3 to 0.4 s for 6 minutes, 3.6 to 3.9 s for 65), streamed to a file
with no memory growth, 10.6 MB a minute. No new playback path and no
second stream, but real work in the controller and real costs, listed
in section 9 for a decision. Every cheaper trick on the player's own
output (unmuting, a hand-over between two streams, a decoder held warm
at the cue) either still cuts the transient or lets audio from before
the cue through (sections 2 to 5). A player with a sink of its own
(section 9) is the fallback.

## What each tool answers

| tool | question | needs |
|---|---|---|
| `seek_probe` | after a seek, where does the first sound come out of the decoder? (`PACE=1`, `RATE=x`: how does a player with only a QAudioBufferOutput pace its buffers?) | no sound card |
| `sink_probe` | where does an unmute (modes `tap`, `clock`) or a resume from a muted hold at the cue (`warm`, `warmtap`) land in what the sound server plays? What does a plain jump from pause play (`plain`, also on a WAV, `SOURCE=buffer` from memory), and a jump while playing (`live`)? | a null sink |
| `handover_probe` | can the cue's first milliseconds come from a buffer decoded ahead, played on a QAudioSink of our own, with the player taking over after them? | a null sink, Qt 6.11 |
| `warm_cost` | what do extra QMediaPlayers, each held muted at a hot cue, cost? | a null sink |
| `pcm_probe` | decode an MP3 to a WAV on the player's timeline: how long, how much memory? | no sound card |
| `align_probe` | does that WAV play the same samples at the same position as the MP3? | no sound card |
| `switch_probe` | what does switching a playing track from its MP3 to its WAV sound like? | a null sink |
| `zero_probe` | played from 0 without a seek, does position() stand for the same sound on the MP3 and on its WAV? | a null sink, Qt 6.11 |
| `measure-unmute.sh` | makes the null sink, records it, runs a probe and the matching analysis (see its header for the safety checks) | PipeWire, `pactl`, `pw-record` |
| `analyse_unmute.py`, `analyse_handover.py`, `analyse_real.py`, `analyse_zero.py` (with `analyse_common.py`) | read the recordings: click files, hand-overs and switches, real music, marker to click | Python 3 |
| `make-probe-audio.py` | writes the probe audio (`--long`: also a 6-minute MP3) | ffmpeg with libmp3lame |
| `probe_common.hpp` | the exact-id device lookup and the interpolated player clock the probes share | |

## Build and run

The probes are not part of the CMake build (they need a sound server or
a null sink, which neither the suite nor CI has). Build them by hand into
a scratch directory, outside the source tree:

    src=$PWD/tools/mp3-seek-preroll-probe
    mkdir -p ~/tmp/mp3-probe && cd ~/tmp/mp3-probe
    for t in seek_probe sink_probe handover_probe warm_cost pcm_probe align_probe switch_probe zero_probe; do
        g++ -std=c++17 -fPIC -O1 -I"$src" "$src/$t.cpp" -o "$t" $(pkg-config --cflags --libs Qt6Multimedia Qt6Core)
    done
    "$src/make-probe-audio.py" . --long
    for f in toneclick toneclick-64k tonegap; do ./pcm_probe $f.mp3 $f.pcm.wav; done
    cp $src/../../tests/fixtures/mp3-click/click-*.mp3 . && ./pcm_probe click-D-lame-44100.mp3 click-D-lame-44100.pcm.wav

`handover_probe` and `zero_probe` need Qt 6.11 (QAudioSink's callback
API). Every command under a table below is run in that directory.
`measure-unmute.sh` sets `LC_ALL=C` (QCoreApplication otherwise sets the
locale's decimal comma), `QT_QPA_PLATFORM=offscreen`,
`QT_MEDIA_BACKEND=ffmpeg` and `QT_AUDIO_BACKEND=pipewire` for the
probes, writes `rec-<mode><TAG>.wav` (the recording), `.log` (the probe's
output) and `.txt` (the analysis, one line a jump) and prints the
analysis's summary line; the counts in the tables are those summary lines.

What keeps the probes off real outputs (the full list is in
`measure-unmute.sh`'s header): the probes play only on the output device
whose id they are given and refuse to start without one; the script
refuses to run unless the sound server is PipeWire and Qt will use its
FFmpeg and PipeWire backends, loads the null sink (`seabass_probe`) with
the lowest session priority, stops before playing if the recorder is not
on the sink's monitor, and checks twice a second that every probe stream
is on the null sink, that the sink still exists and that it has not
become the default output, stopping the run if any of that fails; a run
in which no probe stream was ever seen on the sink fails too. These are
safeguards, not a guarantee: between two checks up to half a second of
sound could reach a wrong sink before the probe is stopped. The user's
configured default output is read at the start and put back as it was if
it changed during the run (the effective default is never set by the
script). On any exit, Ctrl-C included, it stops the probe, then the
recorder, then unloads the sink.

The logs behind every number here (the `.log` and `.txt` next to each
recording, `run<TAG>.log` from `--run`, and the outputs of `pcm_probe`,
`align_probe` and `seek_probe`) are from runs on 2026-11-21 and are not
in the repository: the commands under each table make them again.

All numbers below: Framework laptop (AMD Ryzen 7 7840U, 16 threads),
Ubuntu's Qt 6.11.1 on the system FFmpeg 6.1 (libavformat 60), PipeWire
1.6.0, a 48 kHz null sink (a 1024-frame quantum, 21.33 ms), with a build
and test suite running beside them (load 2.5 to 6). Measured 2026-11-21
unless a section says otherwise.

## 0. Three facts about Qt 6.11.1 the measurements rest on (read in its source)

- **position() steps in 50 ms.** On the FFmpeg backend
  `QMediaPlayer::position()` returns a cached value
  (`QPlatformMediaPlayer::position()`, qplatformmediaplayer_p.h:40) that
  `QFFmpegMediaPlayer::updatePosition()` (qffmpegmediaplayer.cpp:72)
  refreshes from a 50 ms timer (41-43, started on play() at 300) and once
  from setPosition() (59-66). Polling it every millisecond sees a new value
  every 50 ms. The probes therefore judge "the player's clock" by the
  last reported position plus the steady time since it was reported
  (`probe::PlayerClock`), as the app's popup does for its smooth
  scrolling. After a seek it interpolates from the setPosition() call,
  not from when audio starts, so it leads until the first 50 ms report;
  every row that uses it gets at least three reports in the 200 ms
  pre-roll before it matters. The 2026-11-19 `clock` numbers, taken on
  position() itself, were wrong for that reason and are replaced below.
- **Volume and mute act on a whole host buffer.** The player's internal
  QAudioSink is fed in push mode; `QPlatformAudioSinkStream::process()`
  reads `volume()` once for each buffer the sound server asks for and
  applies it to all of it (qaudiosystem_platform_stream_support.cpp:154,
  169). Nothing in QAudioSink or QAudioOutput takes a sample position for
  a volume change, and there is no ramp.
- **A seek empties the reservoir, and nothing refills it.**
  `Demuxer::ensureSeeked()` (playbackengine/qffmpegdemuxer.cpp) calls
  `av_seek_frame(ctx, -1, pos, AVSEEK_FLAG_BACKWARD)` and the decoder is
  flushed. In FFmpeg 6.1 (the version this Qt runs on, n6.1.1 read):
  mp3dec.c's `mp3_seek()` uses the TOC only with `usetoc` or
  `AVFMT_FLAG_FAST_SEEK` and otherwise the generic index, and neither
  decodes anything before the seek point; mpegaudiodec_template.c's
  `mp_flush()` sets `last_buf_size = 0`, and a granule whose
  `main_data_begin` reaches back further than what is buffered is decoded
  from zeroed coefficients (`memset(g->sb_hybrid, 0, ...)`): silence. The
  only options Qt hands `avformat_open_input` are timeout, probesize,
  protocol whitelist (`QT_FFMPEG_PROTOCOL_WHITELIST`) and the low-latency
  flags of QPlaybackOptions; no environment variable or QMediaPlayer
  property reaches the demuxer's or decoder's options.

## 1. What the decoder delivers after a seek (`seek_probe`)

Seek, play, first sample above 0.05 on the buffers' timestamps. The
click files are tests/fixtures/mp3-click/ (one click at 1.5 s) and those
`make-probe-audio.py` writes (clicks at 1.5, 2.5 and 3.5 s; in the
toneclick files the first sound is the tone, not the click).

    for s in 1500 1490 1450 1420 1400 1300; do ./seek_probe FILE $s; done

| file | seek 1500 | 1490 | 1450 | 1420 | 1400 | 1300 |
|---|---|---|---|---|---|---|
| click-A-notag-44100.mp3 (64k, click at 1524.94) | missed | missed | missed | 1524.94 | 1524.94 | 1524.94 |
| click-D-lame-44100.mp3 (64k) | missed | missed | missed | missed | 1499.89 | 1499.89 |
| click-D-lame-48000.mp3 (80k) | missed | missed | missed | 1499.85 | 1499.92 | 1499.92 |
| click-D-lame-mpeg2-22050.mp3 | missed | missed | missed | 1499.77 | 1499.77 | 1499.77 |
| click.mp3 (LAME 192k) | missed | 1500.07 (partly) | 1499.93 | 1499.93 | 1499.93 | 1499.93 |
| toneclick.mp3 (192k): first sound | 1530.97 | 1500.05 | 1478.66 | 1452.49 | 1426.33 | 1319.41 |
| toneclick-64k.mp3: first sound | 1583.31 | 1557.14 | 1531.00 | 1500.05 | 1478.66 | 1372.95 |
| click.wav, click.flac | 1500.00 | | 1500.00 | | | 1500.00 |
| click.m4a (AAC 192k) | 1499.95 | | 1499.95 | | | 1499.95 |

("missed": the first sound is the next click, a second later.) The loss is
MP3 only and depends on the bit rate (the reservoir is a byte count): the
stream is silent for 30 ms after a seek at 192 kbit/s, 73 to 83 ms at 64.
A seek 200 ms before the cue delivers the click on the sample in every
file of the table. WAV, FLAC and AAC lose nothing.

## 2. Where an unmute lands in what is heard (`sink_probe`)

`toneclick.mp3`: a 440 Hz tone at 0.15 under clicks at 1.5, 2.5, 3.5 s, so
any sound before the cue shows as tone. Cue at 1500, 40 jumps a run,
400 ms apart. "Heard before the cue" is how much tone came out before the
click, with the number of jumps that had each value.

    TAG=-192 $src/measure-unmute.sh . toneclick.mp3 1500 40 400 plain tap
    TAG=-192-l$L LEAD=$L $src/measure-unmute.sh . toneclick.mp3 1500 40 400 clock   # L = 0, 10, 22

(The tap row is from a second run of the first command: in the first,
the script's routing check stopped the tap run with a false alarm on a
stream still connecting, since fixed; the plain row is from the first.)

| mode | click not heard | heard before the cue, ms: jumps |
|---|---|---|
| plain (today: seek to the cue) | 40 of 40 | |
| tap (unmute on the buffer that holds the cue) | 0 | 47.6: 36, 69.0: 2, 111.6: 2 |
| clock, unmute when the player's clock reaches the cue | 0 | 5.0: 16, 26.3: 23, 175.7: 1 |
| clock, 10 ms early | 0 | 26.3: 32, 47.6: 8 (and 1 leak) |
| clock, 22 ms early | 0 | 26.3: 9, 47.6: 28, 69.0: 1, 90.3: 2 |

Every value is 5.0 ms (for this cue and seek point) plus a whole number
of 21.33 ms periods (the 175.7 is a jump where the mute itself came too
late). That is the finest an unmute can be placed (section 0: the mute
applies to whole host buffers), and the player's clock leads what is
heard by a little: unmuting when it reads the cue already lets 5 or 26 ms
through. The tap's timestamps say what was decoded, not when it is heard
(the renderer writes to the sink first and hands the buffer to the tap
after), so unmuting on them comes the sink's fill level early. Any lead
lets 5 to 90 ms of what comes before the cue through, with a hard step
from silence to full volume at its start.

## 3. A buffer decoded ahead, handed over to the player (`handover_probe`)

The whole file is decoded with QAudioDecoder (from its start, so the
reservoir is intact). On each jump the player seeks to the cue and plays,
unmuted, while the probe's own QAudioSink (callback API, same device)
plays the decoded audio from the cue and stops, with a 2 ms fade, where
the player's tap shows the player's own audio beginning (first sample
above 0.075, half the tone: the same threshold the analysis counts a gap
by, so the gap is the offset less the 2 ms fade). "Offset" is how much
later the player's stream sounds than the probe's for the same track
time. `DELAY` starts the own audio that much later, placed to the sample
inside the buffer the callback fills, not on a period.

    TAG=-192-d$D DELAY=$D $src/measure-unmute.sh . toneclick.mp3 1500 40 400 handover   # D = 5, 10, 15
    TAG=-192-d0b $src/measure-unmute.sh . toneclick.mp3 1500 40 400 handover
    TAG=-64-d0b $src/measure-unmute.sh . toneclick-64k.mp3 1500 40 400 handover

The DELAY 0 rows are reruns (`d0b`) after the late-splice logging was
fixed. The first 192k DELAY 0 run (`TAG=-192-d0`) had offsets 31.3: 32,
10.0: 6 and -11.4: 2 (the player's stream skipping), gaps 30: 33 and 8:
7, and in 2 jumps 21.3 ms of sound before the click (the leftover period
of section 6); the rerun had neither.

| file, DELAY | splice: the player's audio from | offset, ms: jumps | gap, ms: jumps |
|---|---|---|---|
| toneclick.mp3 (192k), 0 | cue + 32.2 ms; tap told 5 to 6 ms after the jump, never late | 10.0: 12, 31.3: 28 | 8: 12, 30: 28 |
| toneclick.mp3, 5 | same | 10.0: 8, 26.5 to 31.2: 9, 31.3: 23 | 8: 8, 25 to 30: 32 |
| toneclick.mp3, 10 | same | 10.0: 7, 21.7 to 30.7: 19, 31.3: 10; the player's stream skipped 38 to 97 ms: 4 | 8: 9, 20 to 30: 31 |
| toneclick.mp3, 15 | same | 9.5 to 10.0: 12, 17.4 to 29.9: 24; skipped 33 and 139 ms: 2; 2 jumps broken by a dropout | 8: 14, 16 to 29: 24 |
| toneclick-64k.mp3, 0 | cue + 84.5 ms; tap told 56 to 58 ms after the jump, never late | 10.0: 14, 31.3: 26 | 8: 14, 30: 26 |

(The DELAY 5, 10 and 15 runs were made before a logging fix: their audio
and analysis are valid, their count of late splices is not.)

The click is heard on the cue in every measured jump of the rows above,
with nothing before it (the first DELAY 0 run aside, see above). But
the hand-over is a dropout every time: 8 ms at best, mostly 30. With
the own start placed to the sample, the offset no longer moves in whole
periods; it spreads over about 21 ms whatever the delay, because the
player's stream starts on a period of its own with a phase the probe
cannot see (Qt says nowhere when the player's first sample is heard). So
the two were not lined up to within a few ms with this method, and no
public API gives what a better one would need. Overlapping them instead
would play the same audio twice 10 to 30 ms apart (a flam).

## 4. A decoder held warm at the cue (`sink_probe` modes `warm`, `warmtap`)

Mute, seek 200 ms before the cue, play, pause before the cue, wait
300 ms, unmute, play on: no seek at the jump, so the reservoir is intact.

    TAG=-192-l$L LEAD=$L $src/measure-unmute.sh . toneclick.mp3 1500 40 400 warm   # L = 0, 22, 50, 80, 110
    TAG=-192 $src/measure-unmute.sh . toneclick.mp3 1500 40 400 warmtap
    TAG=-64 $src/measure-unmute.sh . toneclick-64k.mp3 1500 40 400 plain warmtap

| how the hold is placed | click not heard | heard before the cue, ms: jumps |
|---|---|---|
| warm: pause when the player's clock reaches the cue (LEAD 0 and 22) | 40 of 40 each | |
| warm, LEAD 50 | 0 | 10.0: 18, 36.1: 22 (and 1 leak) |
| warm, LEAD 80 | 0 | 36.1: 13, 62.2: 27 |
| warm, LEAD 110 | 0 | 67.0: 1, 88.3: 39 |
| warmtap: pause when the tap delivers the buffer before the cue's (toneclick.mp3) | 0 | 10.0: 40 |
| warmtap, toneclick-64k.mp3 | 0 | 10.0: 40 |
| plain, toneclick-64k.mp3 | 40 of 40 | |

Qt's pause stops the renderer feeding the sink; the sink plays out what
it already holds (muted, so a pause that comes after the cue's frame has
been written loses the click with it) and play() goes on with the next
whole decoded frame. What is heard after a resume therefore starts on an
MP3 frame boundary: 10.0 ms is where the cue sits in its frame (1490.0 to
1516.2), 36.1, 62.2 and 88.3 ms are one, two and three frames earlier.
With the tap the pause reached the renderer in time in all 80 jumps, but
that is a race the GUI thread usually wins (the pause must arrive before
the renderer writes the cue's frame, about one frame, 26 ms), not a
guarantee. At best this lets up to one frame (26.1 ms at 44.1 kHz) of
what comes before the cue through.

## 5. What holding players warm costs (`warm_cost`)

The probe's player and COUNT extra QMediaPlayers on `long-320k.mp3` (6
minutes, 320k), each with its own QAudioOutput on the null sink, each
played muted from 200 ms before its cue and paused at it on the player's
clock. Five runs each, output in `run-warm<COUNT>-<i>.log`:

    for n in 1 4 8; do for i in 1 2 3 4 5; do
        TAG=-warm$n-$i $src/measure-unmute.sh --run . ./warm_cost long-320k.mp3 $n seabass_probe
    done; done

| warm players | resident memory added | threads added | all held after |
|---|---|---|---|
| none (the probe's player alone, read before the warm ones start in every run) | 116.2 to 116.5 MB in all | 5 | |
| 1 | 2.36 to 2.51 MB | 5 | 211 to 213 ms |
| 4 | 6.88 to 7.00 MB | 23 | 212 to 214 ms |
| 8 | 13.02 to 13.12 MB | 47 | 212 to 239 ms |

(An earlier set of five, through the previous version of the script,
gave 2.45 to 2.52, 6.92 to 7.01 and 12.99 to 13.13 MB and 212 to 213
ms. COUNT 0 cannot be run through the script any more: no probe stream
ever plays, so the routing check refuses the run.)

Plus one open output stream per warm player, held idle while the track is
loaded. A cue that is jumped to is used up and needs another 211 to 239 ms to be
warm again. Not measured: the switch from the playing player to a warm
one, and whether a re-warming player's start (sections 2 and 4 show the
mute does not always reach the renderer before the first audio) can be
heard over the playing track.

## 6. Decode to PCM at load, play the PCM (`pcm_probe`, `align_probe`, `sink_probe` on the WAV)

The track is decoded once with QAudioDecoder (the same FFmpeg as the
player) into a 16-bit stereo WAV at its own sample rate, and the
unchanged QMediaPlayer plays the WAV. A WAV seeks to the sample (section
1). The decoded buffers are concatenated: Qt 6.11.1's QAudioDecoder
labels them with a sample counter from 0 (its SteppingAudioRenderer
builds the resampler without a start time, qffmpegaudiodecoder.cpp:28-37),
not with the stream's timestamps, so the labels say nothing the count
does not. Concatenation is what puts sample n of the WAV at the seeked
player's track time n / rate, which is why the product must concatenate
deliberately and not place buffers by their labels: if a Qt version ever
labelled them from the stream's timestamps, placing by label would shift
the whole track by the start offset without any error. `pcm_probe`
reports buffers whose label disagrees with the count (none in any run
here). A CI test must pin it: the decoded
tests/fixtures/mp3-click/click-D-lame-44100.mp3 has its click's first
sample above 0.244 (8000 of 32767) at frame 66149 and its peak (15497) at
frame 66150 of the left channel, measured here; another FFmpeg may round
a sample differently, so the test should allow a frame either way and
fail on anything more.

**Timeline.** `align_probe` plays the MP3 and, as a control, the WAV with
a buffer output only and compares 100 ms of each with the WAV's samples
at every lag within 60 ms:

    for f in toneclick toneclick-64k; do
        ./align_probe $f.mp3 $f.pcm.wav 1495@0 1495@10 1495@30 1495@100 1495 2700 3500
        ./align_probe $f.pcm.wav $f.pcm.wav 300 1495 2700 3500
    done
    for f in click-A-notag-44100 click-D-lame-44100 click-D-lame-48000 click-D-lame-mpeg2-22050; do
        ./pcm_probe $f.mp3 $f.pcm.wav && ./align_probe $f.mp3 $f.pcm.wav 1470@0 1495@0 1495@10 1495@30 1495@100 1495
    done
    ./align_probe real-6min.mp3 real-6min.pcm.wav 300 60000 200000 345000

- After any seek from 10 ms on, the MP3 and the WAV play the same
  samples at the same position, lag 0, mean difference under 1e-5 (the
  16-bit rounding), on toneclick.mp3, toneclick-64k.mp3, the four
  fixture files and a real 6-minute 320k MP3 (at 1, 3.3 and 5.75 min).
- Played from the start without a seek (`@0`, or a seek under the start
  offset), the player labels the audio of the MPEG-1 files with a start
  offset (toneclick, toneclick-64k, click-D-lame-44100 and -48000, the
  real track) 25.057 ms early at 44.1 kHz and 23.021 ms at 48 kHz:
  exactly the file's start offset (ffprobe's `start:`, the encoder delay
  FFmpeg trims). After a seek it does not. The MPEG-2 22.05 kHz file
  (start offset 50.1 ms) and click-A-notag (none) agree from the start.
  These are labels on the buffer output (the player's resampler is
  seeded with the first frame's start time, qffmpegaudioframeconverter.cpp:157),
  so what this means for what is heard is settled by `zero_probe` below.
  The WAV has no such case: it is on the seeked MP3's timeline from its
  first sample, which is also QAudioDecoder's, the timeline #50's model
  is measured on (tests/qt_decoder_timeline_test.cpp).
- duration(): the WAV's is the decoded length (4000 ms for toneclick, 345
  926 ms for the real track), the MP3's is FFmpeg's estimate including the
  start offset and padding (4048 ms, 345 965 ms).

**position() from 0.** Does position(), the clock the cues, the ring and
the popup run on, stand for the same sound on the MP3 and on its WAV
when either is played from 0 without a seek? `zero_probe` plays the file
from 0 and, when the player's clock reads 1400 ms, has its own sink play
a marker 30 ms later (placed to the sample); the recording gives the
distance from the marker to the click at 1500. 40 runs a source:

    TAG=-lame-mp3 $src/measure-unmute.sh . click-D-lame-44100.mp3 1400 40 400 zero
    TAG=-lame-wav $src/measure-unmute.sh . click-D-lame-44100.pcm.wav 1400 40 400 zero
    TAG=-tone-mp3 $src/measure-unmute.sh . toneclick.mp3 1400 40 400 zero
    TAG=-tone-wav $src/measure-unmute.sh . toneclick.pcm.wav 1400 40 400 zero

| source, played from 0 | marker to click, ms: min / median / max |
|---|---|
| click-D-lame-44100.mp3 (LAME, 44.1 kHz, start offset 25.06 ms) | 74.2 / 86.0 / 98.5 (40 runs) |
| its WAV | 99.4 / 111.1 / 128.1 (39 runs: in one the click came more than 500 ms after the marker) |
| toneclick.mp3 | 73.8 / 85.4 / 98.3 (40) |
| its WAV | 87.3 / 113.0 / 124.2 (40) |

The spread (about 25 ms within a run) is the method's: the player's
clock is interpolated between 50 ms reports. The medians differ by 25.1
and 27.6 ms: played from 0, the MP3's click sounds about one start
offset earlier, for the same position(), than the WAV's does. After a
seek the two agree (above). So the WAV does not add a shift; it removes
one: today an MP3 played from its start without a seek runs about 25 ms
off the timeline the cues are corrected for (#50's, which is the WAV's)
until the first seek. The rule for the product: the WAV gets exactly the
cue and beat shift the MP3 gets (`m_cueShiftMs`, `m_beatShiftMs`),
computed from the MP3's path, because `playbackShiftFor()`
(src/infrastructure/audio/playback_timeline.cpp:27-33) returns no shift
for a path that is not an MP3 and handing it the WAV's path would land
every cue on a LAME file about 51 ms late; no extra correction at the
start of the file, none after a seek.

**Jumps from pause.** Plain seeks to the cue (sink_probe pauses, seeks,
plays), nothing else:

    TAG=-192 $src/measure-unmute.sh . toneclick.mp3 1500 40 400 plain
    TAG=-64 $src/measure-unmute.sh . toneclick-64k.mp3 1500 40 400 plain
    TAG=-192wav $src/measure-unmute.sh . toneclick.pcm.wav 1500 40 400 plain
    TAG=-64wav $src/measure-unmute.sh . toneclick-64k.pcm.wav 1500 40 400 plain
    TAG=-192wav-buffer SOURCE=buffer $src/measure-unmute.sh . toneclick.pcm.wav 1500 40 400 plain
    TAG=-64wav-buffer SOURCE=buffer $src/measure-unmute.sh . toneclick-64k.pcm.wav 1500 40 400 plain
    TAG=-ctt $src/measure-unmute.sh . clickthentone.wav 1500 40 400 plain
    TAG=-ctt120 $src/measure-unmute.sh . clickthentone.wav 1500 120 400 plain
    TAG=-192wav120 $src/measure-unmute.sh . toneclick.pcm.wav 1500 120 400 plain
    TAG=-192wav-b $src/measure-unmute.sh . toneclick.pcm.wav 1500 40 400 plain      # the "again" rows
    TAG=-64wav-b $src/measure-unmute.sh . toneclick-64k.pcm.wav 1500 40 400 plain
    TAG=-realmp3 REF=real-6min.pcm.wav $src/measure-unmute.sh . real-6min.mp3 134644 40 400 plain
    TAG=-realwav REF=real-6min.pcm.wav $src/measure-unmute.sh . real-6min.pcm.wav 134644 40 400 plain

| source | click not heard | heard before the cue, ms: jumps |
|---|---|---|
| toneclick.mp3 (today) | 40 of 40 | |
| toneclick-64k.mp3 (today) | 40 of 40 | |
| toneclick.pcm.wav, from the file | 0 | 0.0: 40; again (`-192wav-b`): 0.0: 39, 21.4: 1 |
| toneclick-64k.pcm.wav, from the file | 0 | 0.0: 38, 21.4: 2; again (`-64wav-b`): 0.0: 40 |
| toneclick.pcm.wav from memory (`SOURCE=buffer`, a QBuffer via setSourceDevice()) | 0 | 0.0: 39, 21.4: 1 |
| toneclick-64k.pcm.wav from memory | 0 | 0.0: 40 |
| clickthentone.wav (silence before the cue, tone after), 40 jumps | 0 | 0.0: 40 |
| clickthentone.wav, 120 jumps | 0 | 0.0: 119, 21.4: 1 |
| toneclick.pcm.wav, 120 jumps | 0 | 0.0: 120 |

The occasional 21.4 ms (one period) of sound before the click is not
from before the cue: in clickthentone.wav there is only silence before
the cue, yet one of 120 jumps had it. It is left over from the previous
position, one period that plays first when play() follows the seek; it
comes with any source, the MP3 too (where the lost click hides it), and
the WAV neither causes nor cures it. Of all 520 jumps on a WAV, 515
started on the click and 5 with this one leftover period. (The "again"
runs and the two 120-jump runs were made to see whether that leftover
period is real and where it comes from.)

The real track (`real-6min.mp3`): a released 5:46 track at 320k with a
25.06 ms start offset, from the owner's library and not in the repository
(any such file will do), cue on the strongest attack between 100 and 140 s (134 644 ms: the 1 ms
peak rises from 0.27 to 0.94). `analyse_real.py` finds where in the track
each jump's first sound comes from, against the decoded WAV:

| source | first sound, ms from the cue: jumps |
|---|---|
| the MP3 (today) | +11: 39 (the attack's first 11 ms lost; correlation 0.745), -59: 1 (a poor match, correlation 0.585) |
| the WAV | 0: 40 (correlation 0.986) |

The WAV's correlation is close to a self-match by construction: the
reference is that WAV. What this run shows is that a seek in the WAV
lands on its own sample; that the sample is the cue's on the MP3's
timeline rests on `align_probe` above.

**Jumps while playing.** The app usually jumps while a track plays;
sink_probe's `live` mode plays from 2700 ms, in the silent tail of
tonegap (tone and clicks up to 2.6 s, silence after), and 300 ms later
seeks to the cue without pausing, so any tone before the click came from
before the cue:

    TAG=-mp3 $src/measure-unmute.sh . tonegap.mp3 1500 40 400 live
    TAG=-wav $src/measure-unmute.sh . tonegap.pcm.wav 1500 40 400 live

| source | click not heard | heard before the cue, ms: jumps |
|---|---|---|
| tonegap.mp3 (192k, today) | 40 of 40 | |
| tonegap.pcm.wav | 0 | 0.0: 40 |

**Cost of the decode.** `pcm_probe` writes the WAV as the buffers arrive
(header patched at the end); wall time, three runs each, twice (the
second set with the concatenating pcm_probe of this commit, the WAVs
byte-identical to the first set's):

    for f in mix-3min real-6min mix-10min mix-65min; do for i in 1 2 3; do ./pcm_probe $f.mp3 $f.pcm.wav; done; done
    for f in real-6min mix-65min; do ./pcm_probe $f.mp3 $f.mem.wav memory; done

| track (real music, 320k) | decode and write, wall time | WAV | peak memory, streamed | peak memory, held |
|---|---|---|---|---|
| 3 min | 164 to 169 ms; 181 to 183 ms | 31.8 MB | 123.3 to 124.3 MB | |
| 5:46 | 308 to 338 ms; 352 to 365 ms | 61.0 MB | 123.3 to 124.2 MB | 187.4 to 188.1 MB |
| 10 min | 503 to 544 ms; 594 to 606 ms | 105.8 MB | 123.6 to 124.4 MB | |
| 65 min | 3564 to 3681 ms; 3828 to 3915 ms | 688.0 MB | 122.6 to 124.0 MB | 1162 to 1163 MB |

(The 3, 10 and 65 minute tracks are cut with `ffmpeg -ss 600 -t N -c copy`
from a 2-hour 320k DJ mix, not in the repository either; the process stands at 26 MB before the decode,
Qt Multimedia and FFmpeg loaded bring it to about 123 MB.) QAudioDecoder
streams: written to a file, memory does not grow with the length. The
WAV is 10.6 MB a minute at 44.1 kHz; a RIFF WAV holds 4 GB (6.7 hours);
FFmpeg 6.1's wav demuxer also reads RF64, BW64 and W64 (wavdec.c), should
a longer one be wanted. Not measured: the decode on a slower machine. To
it adds reading the whole MP3 from the stick: at 20 to 30 MB/s (a USB 2
stick; an estimate, not measured here) 0.5 to 0.7 s for a 14 MB track,
5 to 8 s for a 156 MB mix.

**Starting before the decode is done.** Playback can start on the MP3 at
once and move to the WAV later. Switching mid-play is heard:

    TAG=-two PCM=toneclick.pcm.wav $src/measure-unmute.sh . toneclick.mp3 0 40 400 switch
    TAG=-same2 SWITCH=same PCM=toneclick.pcm.wav $src/measure-unmute.sh . toneclick.mp3 0 40 400 switch

(`same2` is a rerun: in the first `SWITCH=same` run all 40 jumps were
broken because switch_probe acted on the status the old source reports
as setSource() stops it, before the WAV had loaded; fixed, then rerun.)

| switch | offset of the WAV, ms: jumps | gap, ms: jumps |
|---|---|---|
| a second player, loaded and paused, started at the clock's position | -9.9: 1, -5.3: 15, 16.1: 24 | 0: 9, 20 to 21: 31 |
| the same player, setSource(WAV) then a seek (the WAV loaded in 6 to 10 ms) | -31.2: 1, -9.9: 2, -5.3: 6, 16.1: 30; 1 broken | 0: 2, 20 to 21: 37 |

Both are a 20 ms dropout or a 5 to 10 ms repeat, so the source must not
be switched while the track plays on. The one rule: switch at the first
hot-cue jump after the decode is done, as part of that jump (it is a
discontinuity anyway). That costs the jump the WAV's load time, 6 to 10
ms on the null sink, and the work below; on real WASAPI and Core Audio
devices the reopened output stream is unmeasured and may take longer or
pop.

**What the switch costs in the controller** (read in
src/gui/playback_controller.cpp, nothing changed). setSource() stops the
player, so the controller, which forwards the player's signals, would
pass on playing, stopped, playing; duration 0, then the WAV's (a few
tens of ms shorter than the MP3's estimate); position 0, then the
target. Unfiltered that means the play key flickers, MPRIS announces
Stopped and a length of 0, the level meters drop (they clear on any
state but playing), the popup resyncs for a frame, and on the Qt 6.8
build (macOS 12) the seek-in-place workaround on LoadedMedia (lines
162-178) fires for the WAV too. A WAV that fails to load raises
errorOccurred, which the controller shows as an error (line 186-188),
for what is a background optimisation and must instead fall back to the
MP3 silently. So the switch has to be done inside the controller with
those signals filtered and without going through load(), so that
trackChanged and the duration serial do not fire. The meters themselves
take the WAV's 16-bit buffers as they are; MPRIS, the popup and the ring
read the controller and need nothing once the controller hides the
switch.

## 7. What the probes did not find in Qt 6.11.1

No seek flag, option or environment variable makes the MP3 decoder
pre-roll (section 0); no volume ramp at a sample position (section 0);
QAudioDecoder has no seek, so a cue's neighbourhood can only be decoded
from the start of the file (section 6 says what that costs).

## 8. How a player with only a QAudioBufferOutput paces its buffers (`seek_probe`, `PACE=1`)

    PACE=1 ./seek_probe toneclick.mp3 1300
    PACE=1 RATE=4 ./seek_probe toneclick.mp3 1300

| playback rate | buffer for track time t after the seek arrives at |
|---|---|
| 1 | t ms after play(), every 26.1 ms (one MP3 frame each) |
| 4 | t / 4 ms after play(): 320 ms of track in 81 ms |

The buffers are the same at both rates (timestamps, lengths, first sound
at 1319.4 ms): the rate only drives the renderer's clock, which is the
steady clock (`Renderer::nextTimePoint`), and the buffer output's
resampler does not stretch them. A sink of Seabass's own fed this way
gets the audio just in time on the system clock, not the sound card's.

## 9. The designs, and what they cost

### Decode to PCM (recommended, with these costs)

What it is: when an MP3 is loaded, decode it once with QAudioDecoder
into PCM, and at the first hot-cue jump after that, give the player the
PCM instead of the MP3 and seek there. Jumps in it land on the cue with
the transient whole (section 6). What it costs and what is not known,
none of it measured away:

- **Jumps are only right once the whole MP3 has been read and decoded.**
  About 1 to 1.5 s after loading for a 6-minute track on a USB 2 stick
  (0.5 to 0.7 s to read 14 MB at 20 to 30 MB/s, an estimate, plus 0.3 to
  0.4 s to decode on this laptop), 6 to 12 s for a 65-minute mix. Before that,
  jumps are as today.
- **Every played MP3 is read off the stick a second time**, tracks only
  previewed and tracks played during a save included. Each decode must be
  cancellable on track change, stop and eject: a decoder holding the file
  open would bring back the "target is busy" on eject that stop() clears
  the source to avoid (playback_controller.cpp, stop()).
- **The PCM costs 10.6 MB a minute.** Held in memory (setSourceDevice()
  with a QBuffer measured the same as a file, section 6): no cache file,
  nothing to clean up, fine for tracks under about 15 minutes. On disk,
  in the cache directory (never /tmp, never the stick): it then needs an
  eviction rule, a sweep for files a crash left, and a fallback to the MP3
  when the disk is full.
- **The switch is not invisible.** The controller has to hide the
  player's stop, duration 0 and position 0 from the play key, MPRIS, the
  meters and the popup, keep the Qt 6.8 workaround off it, and fall back
  to the MP3 silently on a load error (section 6). It reopens the output
  stream: 6 to 10 ms on the null sink, unmeasured on real WASAPI and Core
  Audio devices, where it may take longer or pop.
- **About 1 jump in 100 still starts with 21 ms of the previous
  position**, with any source, as today (section 6).
- **Measured on jumps from pause and on jumps while playing** (section
  6), on Linux only.
- **16 bits clamp at full scale**: 2 samples on the real test track, more
  on loud masters. A float WAV avoids it at twice the size (21.2 MB a
  minute).
- **The cue shift**: the PCM takes exactly the MP3's cue and beat shift,
  computed from the MP3's path, with no extra correction at the start or
  after a seek (section 6). Played from 0 the PCM puts the track on #50's
  timeline, where today's MP3 is about 25 ms off until the first seek.
- **The decode must concatenate**, deliberately, and a CI test must pin
  the click's frame in a decoded fixture (section 6).
- **Backend**: the app never sets QT_MEDIA_BACKEND, so Qt's default
  applies: FFmpeg on Linux, Windows and macOS since Qt 6.5, unless the
  user's environment overrides it. That Craft's Qt ships the FFmpeg
  plugin is not verified here: craft-blueprint/qt-apps/seabass/seabass.py:42
  only depends on `libs/qt/qtmultimedia` (that blueprint is Craft's, not
  in this repository), and docs/manual-testing.md:172-173 says Windows
  loads "the FFmpeg plugin and the 19 DLLs behind it", which is the
  evidence there is. The macOS 12 build takes Qt's official 6.8 binaries.
  On each: does QAudioDecoder decode MP3, on the seeked player's timeline
  (`align_probe`), with the same counter labels?

**Estimate: 2 to 3 weeks** with all three platforms and the usual review
rounds: the same figure as the own-sink fallback below. It is still the
better bet: no real-time audio thread, no drift control, no position
clock of Seabass's own, the player's own playback path untouched, and
the core of it (decode, concatenate, the switch's bookkeeping) testable
on CI without a sound device.

**The smallest honest first stage:** Linux and Qt 6.11 only, behind the
Experimental flag; only MP3s that have hot cues; only the loaded track,
the decode cancelled on track change, stop and eject; the PCM in memory,
one track; the switch only at a hot-cue jump; the shift taken from the
MP3; the signal filtering in the controller. Its tests: the decode of
tests/fixtures/mp3-click/click-D-lame-44100.mp3 with the click at frame
66149 (and 66150 for the peak), give or take one; the samples around a
cue equal the decoder's; a failed or cancelled decode leaves the MP3
playing with no error shown; the switch at a jump keeps playing(),
duration and the position the controller reports steady (no stop, no 0)
and fires neither trackChanged nor the duration serial; the cue and beat
shift are the MP3's after the switch; stop() and track change while a
decode runs release the file.

**Variants**, one line each:
- Decode lazily at the first jump: nothing spent while browsing, but the
  first jump in every track stays broken.
- A window around each cue: does not work. QAudioDecoder cannot seek
  (from the start of the file only), and one player cannot splice a
  window into the MP3 (section 3).
- MP3 only: AAC, FLAC and WAV lose nothing after a seek (section 1).

### An own sink (fallback)

QMediaPlayer with only a QAudioBufferOutput
(given the device's format, it resamples), a lock-free ring, and a
QAudioSink of Seabass's own fed from its callback (Qt 6.11 only; the
macOS 12 build on 6.8 keeps today's path). Its real argument: inside one
sink a buffer decoded ahead and the continuing audio can be spliced to
the sample, which two streams cannot (section 3). It changes everything
the player does today: volume and mute become gain in the callback;
pause, resume and the end of a track drain or keep the ring; position()
leads what is heard by the ring and must be recomputed (the popup and
MPRIS read it); the buffers come paced on the system clock (section 8),
so the ring needs drift control; device loss needs the sink recreated.
A variant decodes with libavformat and libavcodec directly instead of
QMediaPlayer: no steady-clock pacing, a pre-roll decoded faster than real
time, sample-exact seeks of its own. The app does not link FFmpeg today
(only tools/mp3_decoder_offset_check does, CMakeLists.txt); doing so means
linking and shipping FFmpeg's libraries with the app on every platform
(Craft may already build them for Qt's plugin, unverified as above; the
macOS 12 build would need its own) and FFmpeg's licence terms for the app itself. Estimate
for either: 2 to 3 weeks, and every platform's audio path to verify on
hardware.

### Not recommended

Unmuting after a pre-roll (5 to 90 ms of earlier
audio, section 2), a hand-over between two streams (an 8 to 30 ms
dropout, section 3), and a decoder held warm at each cue (up to one
frame of earlier audio, a race to win at every hold, 2.5 MB, 5 threads
and an open stream per cue, and its switch-over never measured; sections
4 and 5).
