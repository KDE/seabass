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
every jump land on the cue with the transient whole and nothing from
before it: 40 of 40 on both click files and on a real 6-minute track,
against 40 of 40 lost today (section 6). The WAV plays on the same
timeline as the MP3 after a seek, to the sample. Decoding costs about
0.9 ms of CPU per second of audio (0.3 s for 6 minutes, 3.6 s for 65),
streamed to a cache file with no memory growth, 10.6 MB of disk a
minute. No new playback path and no second stream. Every cheaper trick
on the player's own output (unmuting, a hand-over between two streams,
a decoder held warm at the cue) either still cuts the transient or lets
audio from before the cue through (sections 2 to 5). A player with a
sink of its own (section 9) is the fallback, at several times the cost.

## What each tool answers

| tool | question | needs |
|---|---|---|
| `seek_probe` | after a seek, where does the first sound come out of the decoder? (`PACE=1`, `RATE=x`: how does a player with only a QAudioBufferOutput pace its buffers?) | no sound card |
| `sink_probe` | where does an unmute (modes `tap`, `clock`) or a resume from a muted hold at the cue (`warm`, `warmtap`) land in what the sound server plays? What does a plain jump play (`plain`, also on a WAV, `SOURCE=buffer` from memory)? | a null sink |
| `handover_probe` | can the cue's first milliseconds come from a buffer decoded ahead, played on a QAudioSink of our own, with the player taking over after them? | a null sink, Qt 6.11 |
| `warm_cost` | what do extra QMediaPlayers, each held muted at a hot cue, cost? | a null sink |
| `pcm_probe` | decode an MP3 to a WAV on the player's timeline: how long, how much memory? | no sound card |
| `align_probe` | does that WAV play the same samples at the same position as the MP3? | no sound card |
| `switch_probe` | what does switching a playing track from its MP3 to its WAV sound like? | a null sink |
| `measure-unmute.sh` | makes the null sink, records it, runs a probe and the matching analysis (see its header for the safety checks) | PipeWire, `pactl`, `pw-record` |
| `analyse_unmute.py`, `analyse_handover.py`, `analyse_real.py` (with `analyse_common.py`) | read the recordings: click files, hand-overs and switches, real music | Python 3 |
| `make-probe-audio.py` | writes the probe audio (`--long`: also a 6-minute MP3) | ffmpeg with libmp3lame |
| `probe_common.hpp` | the exact-id device lookup and the interpolated player clock the probes share | |

## Build and run

The probes are not part of the CMake build (they need a sound server or
a null sink, which neither the suite nor CI has). Build them by hand into
a scratch directory, outside the source tree:

    src=$PWD/tools/mp3-seek-preroll-probe
    mkdir -p ~/tmp/mp3-probe && cd ~/tmp/mp3-probe
    for t in seek_probe sink_probe handover_probe warm_cost pcm_probe align_probe switch_probe; do
        g++ -std=c++17 -fPIC -O1 -I"$src" "$src/$t.cpp" -o "$t" $(pkg-config --cflags --libs Qt6Multimedia Qt6Core)
    done
    "$src/make-probe-audio.py" . --long
    for f in toneclick toneclick-64k; do ./pcm_probe $f.mp3 $f.pcm.wav; done

`handover_probe` needs Qt 6.11 (QAudioSink's callback API). Every command
under a table below is run in that directory. `measure-unmute.sh` sets
`LC_ALL=C` (QCoreApplication otherwise sets the locale's decimal comma),
`QT_QPA_PLATFORM=offscreen` and `QT_AUDIO_BACKEND=pipewire` for the
probes, writes `rec-<mode><TAG>.wav` (the recording), `.log` (the probe's
output) and `.txt` (the analysis, one line a jump) and prints the
analysis's summary line; the counts in the tables are those summary lines.

Nothing here plays on a real output. `measure-unmute.sh` refuses to run
unless the sound server is PipeWire and Qt will use its PipeWire backend,
loads the null sink (`seabass_probe`) with the lowest session priority,
puts the default output back if the sink took it anyway (and only then),
stops before playing if the recorder is not on the sink's monitor, checks
twice a second that every probe stream is on the null sink (and stops
the probe if one is not), and on any exit, Ctrl-C included, stops the
probe, then the recorder, then unloads the sink. The probes play only on
the output device whose id they are given and refuse to start without
one.

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
  scrolling. The 2026-11-19 `clock` numbers, taken on position() itself,
  were wrong for that reason and are replaced below.
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

    TAG=-192-d$D DELAY=$D $src/measure-unmute.sh . toneclick.mp3 1500 40 400 handover   # D = 0, 5, 10, 15
    TAG=-64-d0b $src/measure-unmute.sh . toneclick-64k.mp3 1500 40 400 handover

| file, DELAY | splice: the player's audio from | offset, ms: jumps | gap, ms: jumps |
|---|---|---|---|
| toneclick.mp3 (192k), 0 | cue + 32.2 ms; tap told 5 to 6 ms after the jump, never late | 10.0: 12, 31.3: 28 | 8: 12, 30: 28 |
| toneclick.mp3, 5 | same | 10.0: 8, 26.5 to 31.2: 9, 31.3: 23 | 8: 8, 25 to 30: 32 |
| toneclick.mp3, 10 | same | 10.0: 7, 21.7 to 30.7: 19, 31.3: 10; the player's stream skipped 38 to 97 ms: 4 | 8: 9, 20 to 30: 31 |
| toneclick.mp3, 15 | same | 9.5 to 10.0: 12, 17.4 to 29.9: 24; skipped 33 and 139 ms: 2; 2 jumps broken by a dropout | 8: 14, 16 to 29: 24 |
| toneclick-64k.mp3, 0 | cue + 84.5 ms; tap told 56 to 58 ms after the jump, never late | 10.0: 14, 31.3: 26 | 8: 14, 30: 26 |

(The DELAY 5, 10 and 15 runs were made before a logging fix: their audio
and analysis are valid, their count of late splices is not.)

The click is heard on the cue in every jump and nothing before it. But
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
clock. Five runs each:

    $src/measure-unmute.sh --run . ./warm_cost long-320k.mp3 COUNT seabass_probe

| warm players | resident memory added | threads added | all held after |
|---|---|---|---|
| 0 (the probe's player: 115.6 to 115.9 MB, 5 threads) | | | |
| 1 | 2.45 to 2.52 MB | 5 | 212 to 213 ms |
| 4 | 6.92 to 7.01 MB | 23 | 212 to 213 ms |
| 8 | 12.99 to 13.13 MB | 47 | 212 to 213 ms |

Plus one open output stream per warm player, held idle while the track is
loaded. A cue that is jumped to is used up and needs another 212 ms to be
warm again. Not measured: the switch from the playing player to a warm
one, and whether a re-warming player's start (sections 2 and 4 show the
mute does not always reach the renderer before the first audio) can be
heard over the playing track.

## 6. Decode to PCM at load, play the PCM (`pcm_probe`, `align_probe`, `sink_probe` on the WAV)

The track is decoded once with QAudioDecoder (the same FFmpeg as the
player) into a 16-bit stereo WAV at its own sample rate, the buffers
placed by their timestamps, and the unchanged QMediaPlayer plays the WAV.
A WAV seeks to the sample (section 1).

**Timeline.** `align_probe` plays the MP3 and, as a control, the WAV with
a buffer output only and compares 100 ms of each with the WAV's samples
at every lag within 60 ms:

    ./align_probe toneclick.mp3 toneclick.pcm.wav 1495@0 1495@10 1495@30 1495@100 1495 2700 3500
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
  The WAV has no such case: it is on the seeked MP3's timeline from its
  first sample, which is also QAudioDecoder's, the timeline #50's model
  is measured on (tests/qt_decoder_timeline_test.cpp).
- duration(): the WAV's is the decoded length (4000 ms for toneclick, 345
  926 ms for the real track), the MP3's is FFmpeg's estimate including the
  start offset and padding (4048 ms, 345 965 ms).

**Jumps.** Plain seeks to the cue, nothing else:

    TAG=-192 $src/measure-unmute.sh . toneclick.mp3 1500 40 400 plain
    TAG=-64 $src/measure-unmute.sh . toneclick-64k.mp3 1500 40 400 plain
    TAG=-192wav $src/measure-unmute.sh . toneclick.pcm.wav 1500 40 400 plain
    TAG=-64wav $src/measure-unmute.sh . toneclick-64k.pcm.wav 1500 40 400 plain
    TAG=-192wav-buffer SOURCE=buffer $src/measure-unmute.sh . toneclick.pcm.wav 1500 40 400 plain
    TAG=-64wav-buffer SOURCE=buffer $src/measure-unmute.sh . toneclick-64k.pcm.wav 1500 40 400 plain
    TAG=-ctt $src/measure-unmute.sh . clickthentone.wav 1500 40 400 plain
    TAG=-ctt120 $src/measure-unmute.sh . clickthentone.wav 1500 120 400 plain
    TAG=-realmp3 REF=real-6min.pcm.wav $src/measure-unmute.sh . real-6min.mp3 134644 40 400 plain
    TAG=-realwav REF=real-6min.pcm.wav $src/measure-unmute.sh . real-6min.pcm.wav 134644 40 400 plain

| source | click not heard | heard before the cue, ms: jumps |
|---|---|---|
| toneclick.mp3 (today) | 40 of 40 | |
| toneclick-64k.mp3 (today) | 40 of 40 | |
| toneclick.pcm.wav, from the file | 0 | 0.0: 40; again: 0.0: 39, 21.4: 1 |
| toneclick-64k.pcm.wav, from the file | 0 | 0.0: 38, 21.4: 2; again: 0.0: 40 |
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
started on the click and 5 with this one leftover period.

The real track (`real-6min.mp3`): a released 5:46 track at 320k with a
25.06 ms start offset, from the owner's library and not in the repository
(any such file will do), cue on the strongest attack between 100 and 140 s (134 644 ms: the 1 ms
peak rises from 0.27 to 0.94). `analyse_real.py` finds where in the track
each jump's first sound comes from, against the decoded WAV:

| source | first sound, ms from the cue: jumps |
|---|---|
| the MP3 (today) | +11: 39 (the attack's first 11 ms lost), -59: 1 (a poor match, correlation 0.59) |
| the WAV | 0: 40 (correlation 0.986) |

**Cost of the decode.** `pcm_probe` writes the WAV as the buffers arrive
(header patched at the end), three runs each:

    ./pcm_probe FILE OUT.wav            # streamed to the file
    ./pcm_probe FILE OUT.wav memory     # everything held, written at the end

| track (real music, 320k) | decode and write | WAV | peak memory, streamed | peak memory, held |
|---|---|---|---|---|
| 3 min | 164 to 169 ms | 31.8 MB | 123.3 to 123.6 MB | |
| 5:46 | 308 to 338 ms | 61.0 MB | 123.3 to 123.8 MB | 187.4 MB |
| 10 min | 503 to 544 ms | 105.8 MB | 123.6 to 123.7 MB | |
| 65 min | 3564 to 3681 ms | 688.0 MB | 123.4 to 123.8 MB | 1163 MB |

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

| switch | offset of the WAV, ms: jumps | gap, ms: jumps |
|---|---|---|
| a second player, loaded and paused, started at the clock's position | -9.9: 1, -5.3: 15, 16.1: 24 | 0: 9, 20 to 21: 31 |
| the same player, setSource(WAV) then a seek (the WAV loaded in 6 to 10 ms) | -31.2: 1, -9.9: 2, -5.3: 6, 16.1: 30; 1 broken | 0: 2, 20 to 21: 37 |

Both are a 20 ms dropout or a 5 to 10 ms repeat. But a hot cue jump is a
discontinuity anyway: switching the source at the first jump (or seek, or
pause) after the decode is done costs that one jump the WAV's load time
(6 to 10 ms here) and nothing else.

**What else it touches** (read in src/gui, nothing changed): the level
meters read the QAudioBufferOutput, which delivers the WAV's buffers
(16-bit, which `meterBuffer()` takes); MPRIS reads the controller's
position, duration and Seeked, all from the player; the waveform popup
and the ring interpolate `controller.position`. None needs a change
beyond what the timeline difference above means: a LAME MPEG-1 track
played from its start without a seek sounds one start offset (25 ms)
later on the WAV than on the MP3 today, which brings it onto the timeline the cues are already
corrected for, and duration() reports the decoded length.

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

**Decode to PCM (recommended).** In PlaybackController, for MP3 only and
behind the Experimental flag: when an MP3 is loaded, decode it on a
worker thread with QAudioDecoder into a WAV in the app's cache directory
(never on the stick), placed by the buffers' timestamps as `pcm_probe`
does; play the MP3 meanwhile; at the first seek, jump or stop after the
WAV is complete, set the player's source to it (keeping the volume and
the play state) and seek there. Keep the original path for `sameFile`,
MPRIS and the title; drop the WAV on track change or keep a few, bounded.
Estimate: 1 to 1.5 weeks with the usual review rounds. Tests, all on CI
without a sound device: the decode of the mp3-click fixtures, asserting
the click's sample index and the samples around a cue (against the
timeline qt_decoder_timeline_test already pins); that a WAV shorter than
the MP3 or a failed decode leaves the MP3 playing; that the source moves
to the WAV only at a seek, and that position(), duration and the cue
shift stay right across the move (QML test with the controller). To
check before it ships, on each platform's own FFmpeg (Craft's Qt 6.10 and
later ships the FFmpeg plugin, qtmultimedia in craft-blueprint; the macOS
12 build takes Qt's official 6.8 binaries, whose FFmpeg plugin carries its
own FFmpeg): that QAudioDecoder decodes MP3 there at all and on the
seeked player's timeline (`align_probe`, including the start offset);
the decode time on the slowest supported machine and the read time from
a real stick; a hot cue on a transient by ear on real outputs on
Windows (WASAPI) and macOS (Core Audio), which the null sink cannot
stand in for; and Qt 6.8's QAudioDecoder and setSource() behaving the
same (6.8 has both).

**An own sink (fallback).** QMediaPlayer with only a QAudioBufferOutput
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
(Craft builds them already for Qt's plugin, the macOS 12 build would
need its own) and FFmpeg's licence terms for the app itself. Estimate
for either: 2 to 3 weeks, and every platform's audio path to verify on
hardware.

**Not recommended:** unmuting after a pre-roll (5 to 90 ms of earlier
audio, section 2), a hand-over between two streams (an 8 to 30 ms
dropout, section 3), and a decoder held warm at each cue (up to one
frame of earlier audio, a race to win at every hold, 2.5 MB, 5 threads
and an open stream per cue, and its switch-over never measured; sections
4 and 5).
