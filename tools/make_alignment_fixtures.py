#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Write tests/fixtures/alignment/: pairs of copies of one song with a known shift.

AudioAlignmentProbe::shiftMs(a, b) answers where a sound sits in b minus
where it sits in a, on the decoder's timeline, or "unknown". This makes
pairs whose right answer is known without asking that code: signals made
here, copies of them with silence put in front, the start trimmed, a
longer tail, a section cut out, another gain, the other polarity, another
sample rate, another codec, and loops that repeat bar for bar.

The truth of every pair is established here, by two routes that must
agree:

  * construction: where the source's time 0 sits in each file;
  * measurement: every file is decoded with the ffmpeg command line and
    each marker event (a short tone burst, its waveform known) is found
    in the decoded samples by a matched filter, to a fraction of a
    sample. For an encoded file that is the only route, so an encoder's
    delay and priming, and whether the decoder trims them, are measured
    and not assumed.

Writes the audio, files.tsv (every file: rate, MP3 class and
cueTimelineOffsetMs as mp3_lead_in_dump reads them, marker times on the
decoder's timeline) and pairs.tsv (every pair: the true shift or
"unknown", the answers also accepted, and why). README.md explains them.

usage: make_alignment_fixtures.py LEAD_IN_DUMP [OUT_DIR]

LEAD_IN_DUMP is the mp3_lead_in_dump tool from a Seabass build directory.
Needs python3 with numpy, the ffmpeg command line (with libmp3lame and
aac) and libmp3lame itself (through ctypes, as make-click-mp3s.py does).
Every random choice is seeded: the output is the same on every run with
the same ffmpeg and libmp3lame.
"""

import math
import os
import re
import subprocess
import sys
import tempfile
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "mp3-offset-probe"))
import lame_encode  # noqa: E402

FFMPEG = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y"]
# No version strings or dates in the files, so a rerun with the same
# encoders writes the same bytes.
BITEXACT = ["-map_metadata", "-1", "-fflags", "+bitexact", "-flags:a", "+bitexact"]

# The loops: 120 BPM, so a beat is 500 ms and a bar 2000 ms; at 8 kHz a
# sixteenth is 1000 samples and every hit starts on a whole sample. A low
# rate because a loop does not compress: 8 kHz halves what they weigh.
LOOP_RATE = 8000
BEAT_S = 0.5
BAR_S = 2.0
STEP = int(LOOP_RATE * BAR_S / 16)

BASE_S = 4.5  # every short copy's length before its edit
LONG_S = 85.0  # long enough for the start, middle and end windows to differ

# A marker event: a burst of a few tones with a sharp onset and a short
# decay, the same at every sample rate (a function of time, not a run of
# samples), and different from every other event.
EVENT_S = 0.12


class Event:
    def __init__(self, rng, gain=1.0):
        self.freqs = rng.uniform(250.0, 3500.0, 6)
        self.phases = rng.uniform(0.0, 2.0 * math.pi, 6)
        self.amps = rng.uniform(0.5, 1.0, 6)
        self.gain = gain

    def wave(self, tau):
        """Samples at times tau (s) from the onset; 0 outside the event."""
        inside = (tau >= 0.0) & (tau < EVENT_S)
        t = np.where(inside, tau, 0.0)
        tones = np.zeros_like(t)
        for f, p, a in zip(self.freqs, self.phases, self.amps):
            tones += a * np.sin(2.0 * math.pi * f * t + p)
        envelope = (1.0 - np.exp(-t / 0.0004)) * np.exp(-t / 0.025)
        return np.where(inside, self.gain * 0.12 * tones * envelope, 0.0)


class Source:
    """A signal as a function of time, with its marker events.

    `events` is a list of (id, onset in s, Event). `extra(t)` adds what is
    not an event (music, a pad); None for nothing."""

    def __init__(self, name, events, extra=None):
        self.name = name
        self.events = events
        self.extra = extra

    def render(self, rate, start_s, seconds):
        """`seconds` of the source from `start_s` on, sampled at `rate`."""
        n = int(round(seconds * rate))
        t = start_s + np.arange(n) / rate
        out = np.zeros(n)
        if self.extra is not None:
            out += self.extra(t)
        for _, onset, event in self.events:
            lo = max(0, int(math.floor((onset - start_s) * rate)) - 1)
            hi = min(n, int(math.ceil((onset + EVENT_S - start_s) * rate)) + 1)
            if lo < hi:
                out[lo:hi] += event.wave(t[lo:hi] - onset)
        return out


def event_times(rng, first, last, count, spacing):
    """`count` onsets in [first, last], on a 40 ms grid (a whole number of
    samples at every rate used here), at least `spacing` apart, irregular."""
    grid = 0.04
    step = spacing + grid
    room = (last - first) - step * (count - 1)
    assert room > 0, "no room for %d events" % count
    spread = np.sort(rng.uniform(0.0, room, count))
    times = first + spread + step * np.arange(count)
    return [round(float(np.floor(t / grid + 1e-9) * grid), 6) for t in times]


def burst_source(name, seed, seconds, count, spacing=0.28, first=0.4):
    rng = np.random.default_rng(seed)
    times = event_times(rng, first, seconds - 0.3, count, spacing)
    events = [("%s%d" % (name, i + 1), t, Event(rng, rng.uniform(0.6, 1.0))) for i, t in enumerate(times)]
    return Source(name, events)


def tones(seed, count, low, high, level):
    """Tones at random frequencies, each swelling and fading at its own
    slow rate: nothing in it repeats within a file (alignment_probe_test's
    music())."""
    rng = np.random.default_rng(seed)
    spec = [(rng.uniform(low, high), rng.uniform(0.05, 1.5), rng.uniform(0, 2 * math.pi), rng.uniform(0, 2 * math.pi))
            for _ in range(count)]

    def f(t):
        out = np.zeros_like(t)
        for freq, swell, phi, psi in spec:
            amp = 0.5 + 0.5 * np.sin(2 * math.pi * swell * t + psi)
            out += amp * np.sin(2 * math.pi * freq * t + phi)
        return level * out / count

    return f


# ---- the loops ---------------------------------------------------------------

def loop_hits(seed, rate=LOOP_RATE):
    """The drum sounds, as runs of samples at `rate`. Each ends inside the
    bar it starts in, so bar after bar is the same samples."""
    rng = np.random.default_rng(seed)
    r = rate

    def t(seconds):
        return np.arange(int(seconds * r)) / r

    tk = t(0.28)
    freq = 50.0 + 90.0 * np.exp(-tk / 0.03)
    kick = 0.55 * np.sin(2 * math.pi * np.cumsum(freq) / r) * np.exp(-tk / 0.09)
    th = t(0.06)
    hat = np.diff(rng.standard_normal(len(th) + 1)) * 0.06 * np.exp(-th / 0.015)
    to = t(0.2)
    open_hat = np.diff(rng.standard_normal(len(to) + 1)) * 0.05 * np.exp(-to / 0.05)
    tc = t(0.15)
    clap = rng.standard_normal(len(tc)) * 0.12 * np.exp(-tc / 0.03)
    ts = t(0.22)
    stab = 0.15 * (np.sin(2 * math.pi * 440 * ts) + np.sin(2 * math.pi * 659.25 * ts)) * np.exp(-ts / 0.06)
    return {"kick": kick, "hat": hat, "open": open_hat, "clap": clap, "stab": stab}


# The long loops' kit: kick and stab, tones with silence between them,
# at 4 kHz: 135 s files, and the directory is to stay small. (The marker
# bursts' upper tones fold over at that rate; the matched filter that
# finds them is sampled the same way, so where they are is still exact.)
TONAL_KIT = ("kick", "stab")
DJ_RATE = 4000
# And a quarter as loud: two bits a sample fewer to store, nothing lost to
# a correlation.
LOOP_LEVEL = 0.25

# One bar in sixteenths. The stab on the one makes the bar, and not the
# beat or the half bar, the loop's period.
PATTERN = {"kick": [0, 4, 8, 12], "hat": [1, 3, 5, 7, 9, 11, 13, 15], "open": [2, 6, 10, 14],
           "clap": [4, 12], "stab": [0]}


def render_loop(seconds, start_s=0.0, vary_seed=None, spread=0.25, ghosts=True, kit=None, rate=LOOP_RATE):
    """The drum loop from `start_s` for `seconds`. With `vary_seed`, every
    bar differs a little: each hit's level by up to `spread` (a quarter),
    and with `ghosts` ghost hats here and there. Always rendered from the
    first bar and then cut, so a varied loop has the same bars whatever
    stretch of it is asked for. `kit` names the sounds played (all of
    PATTERN by default): the tonal ones leave silence between hits, which
    FLAC stores in next to nothing, where noise does not compress."""
    hits = loop_hits(7, rate)
    step_samples = int(rate * BAR_S / 16)
    skip = int(round(start_s * rate))
    n = skip + int(round(seconds * rate))
    rng = np.random.default_rng(vary_seed) if vary_seed is not None else None
    out = np.zeros(n + rate)
    for step in range(n // step_samples + 1):
        in_bar = step % 16
        at = step * step_samples
        for name, where in PATTERN.items():
            level = rng.uniform(1.0 - spread, 1.0 + spread) if rng is not None else 1.0
            if in_bar in where and (kit is None or name in kit):
                out[at:at + len(hits[name])] += 0.6 * level * hits[name]
        if rng is not None and ghosts:
            ghost = rng.uniform() < 0.3 and in_bar % 2 == 0
            level = rng.uniform(0.3, 0.6)
            if ghost:
                out[at:at + len(hits["hat"])] += 0.6 * level * hits["hat"]
    return out[skip:n]


# ---- writing -----------------------------------------------------------------

def pcm16(samples):
    peak = np.max(np.abs(samples)) if len(samples) else 0.0
    assert peak < 0.99, "clipping: peak %.3f" % peak
    return np.round(samples * 32767.0).astype("<i2")


def write_wav(path, rate, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm16(samples).tobytes())


def encode(path, rate, samples, kind):
    """Write `samples` to `path` as `kind`: wav, flac, m4a, mp3-B (ffmpeg's
    own Info frame), mp3-A (libmp3lame, no Info frame) or mp3-D
    (libmp3lame with a LAME tag whose CRC is right)."""
    if kind == "wav":
        write_wav(path, rate, samples)
        return
    if kind in ("mp3-A", "mp3-D"):
        lame_encode.encode(path, rate, 1, "mono", "64", notag=(kind == "mp3-A"), pcm=[int(v) for v in pcm16(samples)])
        return
    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "in.wav")
        write_wav(wav, rate, samples)
        codec = {"flac": ["-c:a", "flac", "-compression_level", "12"],
                 "m4a": ["-c:a", "aac", "-b:a", "48k"],
                 "mp3-B": ["-c:a", "libmp3lame", "-b:a", "48k"]}[kind]
        subprocess.run(FFMPEG + ["-i", wav] + codec + BITEXACT + [path], check=True)


def decode(path):
    """The file as ffmpeg decodes it, mono float, at its own rate."""
    rate = int(subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
                               "stream=sample_rate", "-of", "default=nw=1:nk=1", path],
                              check=True, capture_output=True, text=True).stdout.strip())
    raw = subprocess.run(FFMPEG + ["-i", path, "-f", "f32le", "-ac", "1", "-"], check=True,
                         capture_output=True).stdout
    return rate, np.frombuffer(raw, dtype="<f4").astype(np.float64)


def locate(samples, rate, event, expected_s, search_s=0.06):
    """Where `event` starts in `samples`, in s, to a fraction of a sample:
    the peak of a matched filter within `search_s` of `expected_s`, then a
    parabola through it and its neighbours. Also the normalised
    correlation there (1 for the event exactly, alone)."""
    m = int(round(EVENT_S * rate))
    template = event.wave(np.arange(m) / rate)
    lo = max(0, int((expected_s - search_s) * rate))
    hi = min(len(samples) - m, int((expected_s + search_s) * rate))
    if hi <= lo + 2:
        return None, 0.0
    segment = samples[lo:hi + m]
    corr = np.correlate(segment, template, mode="valid")
    j = int(np.argmax(np.abs(corr)))
    sign = 1.0 if corr[j] >= 0 else -1.0
    frac = 0.0
    if 0 < j < len(corr) - 1:
        y0, y1, y2 = sign * corr[j - 1], sign * corr[j], sign * corr[j + 1]
        denom = y0 - 2 * y1 + y2
        if denom != 0:
            frac = 0.5 * (y0 - y2) / denom
    window = segment[j:j + m]
    norm = abs(corr[j]) / math.sqrt(np.dot(template, template) * max(np.dot(window, window), 1e-30))
    return (lo + j + frac) / rate, norm


def lead_in(dump, path):
    """mp3_lead_in_dump's class and engineToRekordboxOffsetMs for an MP3."""
    out = subprocess.run([dump, path], check=True, capture_output=True, text=True).stdout
    case = re.search(r"case (\w)", out).group(1)
    offset = float(re.search(r"engineToRekordboxOffsetMs=(-?[0-9.]+)", out).group(1))
    return case, offset


# ---- the set -----------------------------------------------------------------

class File:
    def __init__(self, name, source, rate, kind, offset_samples, samples, cut=None):
        self.name = name
        self.source = source
        self.rate = rate
        self.kind = kind
        # Where the source's time 0 sits in this file, in samples.
        self.offset_samples = offset_samples
        self.samples = samples
        # (from_s, length_s) of the source taken out of this copy.
        self.cut = cut
        # (at_s, length_s): this copy repeats the `length_s` of the source
        # before `at_s` there, so everything from `at_s` on is that much
        # later.
        self.insert = None
        self.events = {}  # id: decoder time in s, measured
        self.residual_ms = 0.0  # measured minus constructed, the codec's part
        self.mp3_class = "-"
        self.offset_ms = 0.0  # cueTimelineOffsetMs

    @property
    def offset_s(self):
        return self.offset_samples / self.rate

    def constructed(self, onset):
        """Where the source's onset sits in this file, or None if it is not
        there whole."""
        t = onset
        if self.insert is not None:
            at, length = self.insert
            if at - EVENT_S < onset < at:
                return None
            if onset >= at:
                t += length
        if self.cut is not None:
            start, length = self.cut
            if start - EVENT_S < onset < start + length:
                return None
            if onset >= start + length:
                t -= length
        t += self.offset_s
        duration = len(self.samples) / self.rate
        return t if 0.0 <= t and t + EVENT_S <= duration else None


def build(out):
    files = {}

    def add(name, source, rate, kind, offset_samples, samples, cut=None):
        f = File(name, source, rate, kind, offset_samples, samples, cut)
        files[name] = f
        encode(os.path.join(out, name), rate, samples, kind)
        return f

    def lead(rate, ms):
        return int(round(ms * rate / 1000.0))

    def with_lead(rate, samples, n):
        return np.concatenate([np.zeros(n), samples])

    # (a) Tone bursts at irregular times, nothing else: the plainest
    # thing to line up, and digital silence between, which FLAC stores in
    # next to nothing.
    a = burst_source("a", 101, BASE_S, 8)
    other = burst_source("o", 202, BASE_S, 8)
    tail = burst_source("t", 303, 2.0, 3, first=0.2)
    r = 44100
    base = a.render(r, 0.0, BASE_S)
    add("a-ref.flac", a, r, "flac", 0, base)
    for ms in (21, 46, 675, 1300):
        n = lead(r, ms)
        add("a-lead%d.flac" % ms, a, r, "flac", n, with_lead(r, base, n))
    n = lead(r, 300)
    add("a-trim300.flac", a, r, "flac", -n, base[n:])
    add("a-tail.flac", a, r, "flac", 0, np.concatenate([base, tail.render(r, 0.0, 2.0)]))
    add("a-gain6.flac", a, r, "flac", 0, base * 10 ** (-6 / 20))
    add("a-invert.flac", a, r, "flac", 0, -base)
    add("a-other.flac", other, r, "flac", 0, other.render(r, 0.0, BASE_S))
    cut_at, cut_len = 1.6, 0.4
    add("a-cut.flac", a, r, "flac", 0,
        np.concatenate([base[:lead(r, cut_at * 1000)], base[lead(r, (cut_at + cut_len) * 1000):]]),
        cut=(cut_at, cut_len))
    n48 = lead(48000, 120)
    add("a-48k-lead120.flac", a, 48000, "flac", n48, with_lead(48000, a.render(48000, 0.0, BASE_S), n48))
    # WAV at 11.025 kHz: WAV stores silence as dearly as sound, and a
    # lower rate keeps the pair small (and puts the analysis rate's other
    # side in the set: this one is resampled up to it).
    w = 11025
    wbase = a.render(w, 0.0, BASE_S)
    add("a-ref.wav", a, w, "wav", 0, wbase)
    n = lead(w, 160)
    add("a-lead160.wav", a, w, "wav", n, with_lead(w, wbase, n))
    # Encoded: each MP3 class ffmpeg and libmp3lame can write, and AAC.
    add("a-ref-B.mp3", a, r, "mp3-B", 0, base)
    n = lead(r, 46)
    add("a-lead46-A.mp3", a, r, "mp3-A", n, with_lead(r, base, n))
    n = lead(r, 21)
    add("a-lead21-D.mp3", a, r, "mp3-D", n, with_lead(r, base, n))
    add("a-ref.m4a", a, r, "m4a", 0, base)
    n = lead(r, 160)
    add("a-lead160.m4a", a, r, "m4a", n, with_lead(r, base, n))

    # (b) A loop that repeats exactly, bar for bar: no position in it is
    # told from the same position a bar on.
    b = Source("b", [])
    loop = render_loop(BASE_S)
    add("b-ref.flac", b, LOOP_RATE, "flac", 0, loop)
    n = lead(LOOP_RATE, 730)
    add("b-lead730.flac", b, LOOP_RATE, "flac", n, with_lead(LOOP_RATE, loop, n))
    # One more bar of the loop in front: the same samples as b-ref from
    # its second bar on.
    add("b-bar.flac", b, LOOP_RATE, "flac", lead(LOOP_RATE, 2000), render_loop(BASE_S + BAR_S))

    # The same loop with 1% of its energy in something that does not
    # repeat (a pad of slow tones): the case the 946c85f2 review
    # reproduced. Two windows of one longer stretch, the second 1.3 s on,
    # so the copies are the same length.
    pad = tones(404, 8, 150.0, 1200.0, 1.0)
    longer = render_loop(BASE_S + 1.3)
    t = np.arange(len(longer)) / LOOP_RATE
    pad_wave = pad(t)
    pad_wave *= math.sqrt(0.01 * np.dot(longer, longer) / np.dot(pad_wave, pad_wave))
    near = longer + pad_wave
    nsrc = Source("n", [])
    n13 = lead(LOOP_RATE, 1300)
    nlen = lead(LOOP_RATE, BASE_S * 1000)
    add("n-ref.flac", nsrc, LOOP_RATE, "flac", 0, near[:nlen])
    add("n-eq-1300.flac", nsrc, LOOP_RATE, "flac", -n13, near[n13:n13 + nlen])

    # (c) The realistic case: the loop with every bar a little different,
    # a pad that does not repeat, and tone bursts at irregular times (the
    # percussion that is not part of the loop; also the markers).
    crng_events = burst_source("c", 505, BASE_S + 1.3, 7, spacing=0.3, first=0.3)
    cpad = tones(606, 8, 150.0, 1500.0, 0.05)

    def realistic(start_s, seconds):
        drums = render_loop(seconds, start_s, vary_seed=909)
        return drums + crng_events.render(LOOP_RATE, start_s, seconds) \
            + cpad(start_s + np.arange(len(drums)) / LOOP_RATE)

    csrc = crng_events
    creal = realistic(0.0, BASE_S)
    add("c-ref.flac", csrc, LOOP_RATE, "flac", 0, creal)
    for ms in (620, 1300):
        n = lead(LOOP_RATE, ms)
        add("c-lead%d.flac" % ms, csrc, LOOP_RATE, "flac", n, with_lead(LOOP_RATE, creal, n))
    add("c-eq-1300.flac", csrc, LOOP_RATE, "flac", -n13, realistic(1.3, BASE_S))

    # (d) Music after a long silent intro: three seconds of digital
    # silence, then slow tones with a few bursts in them.
    dmusic = tones(707, 12, 80.0, 4000.0, 0.25)
    drng = np.random.default_rng(808)
    dtimes = event_times(drng, 3.4, 3.0 + BASE_S - 0.3, 5, 0.4)
    dsrc = Source("d", [("d%d" % (i + 1), t, Event(drng, 0.8)) for i, t in enumerate(dtimes)],
                  extra=lambda t: np.where(t >= 3.0, dmusic(t), 0.0))
    dr = 11025
    dbase = dsrc.render(dr, 0.0, 3.0 + BASE_S)
    add("d-ref.flac", dsrc, dr, "flac", 0, dbase)
    n = lead(dr, 675)
    add("d-intro675.flac", dsrc, dr, "flac", n, with_lead(dr, dbase, n))
    add("d-intro675.m4a", dsrc, dr, "m4a", n, with_lead(dr, dbase, n))

    # Long: start, middle and end windows each their own stretch. Bursts
    # alone, so FLAC keeps the files small however long they are.
    L = burst_source("L", 1001, LONG_S, 40, spacing=0.6)
    Lt = burst_source("T", 1002, 6.0, 5, first=0.2)
    lr = 11025
    lbase = L.render(lr, 0.0, LONG_S)
    add("L-ref.flac", L, lr, "flac", 0, lbase)
    add("L-tail.flac", L, lr, "flac", 0, np.concatenate([lbase, Lt.render(lr, 0.0, 6.0)]))
    lcut_at, lcut_len = 42.5, 0.4
    add("L-cut.flac", L, lr, "flac", 0,
        np.concatenate([lbase[:lead(lr, lcut_at * 1000)], lbase[lead(lr, (lcut_at + lcut_len) * 1000):]]),
        cut=(lcut_at, lcut_len))

    # (j) A DJ track: 45 s of a beat-only loop, 45 s of sounds that do not
    # repeat (tone bursts at irregular times, the markers, with silence
    # between, which FLAC stores in next to nothing), 45 s of the loop
    # again, so the start and end windows are all loop and the middle has
    # none. The loop varies by 3% a bar, so the true lag is the best one
    # and a bar away is nearly as good: the start and end windows are
    # torn, the middle settles them. A marker in the intro and one in the
    # outro, among the loop, for a cue placed there.
    J_S, J_PART = 135.0, 45.0
    jrng = np.random.default_rng(1101)
    jtimes = event_times(jrng, J_PART + 0.5, 2 * J_PART - 0.5, 24, 0.8)
    jevents = [("j%d" % (i + 1), t, Event(jrng, jrng.uniform(0.6, 1.0))) for i, t in enumerate(jtimes)]
    jevents.insert(0, ("jintro", 20.3, Event(jrng, 0.6)))
    jevents.append(("joutro", 2 * J_PART + 20.3, Event(jrng, 0.6)))

    def dj(seconds):
        t = np.arange(int(round(seconds * DJ_RATE))) / DJ_RATE
        near = LOOP_LEVEL * render_loop(seconds, vary_seed=1103, spread=0.03, ghosts=False, kit=TONAL_KIT,
                                        rate=DJ_RATE)
        middle = (t >= J_PART) & (t < 2 * J_PART)
        return np.where(middle, 0.0, near)

    jsrc = Source("j", jevents)
    jbase = dj(J_S) + jsrc.render(DJ_RATE, 0.0, J_S)
    add("j-ref.flac", jsrc, DJ_RATE, "flac", 0, jbase)
    for ms in (21, 1300):
        n = lead(DJ_RATE, ms)
        add("j-lead%d.flac" % ms, jsrc, DJ_RATE, "flac", n, with_lead(DJ_RATE, jbase, n))
    # A whole bar missing from the middle of one copy.
    jcut_at, jcut_len = 66.0, BAR_S
    add("j-barcut.flac", jsrc, DJ_RATE, "flac", 0,
        np.concatenate([jbase[:lead(DJ_RATE, jcut_at * 1000)], jbase[lead(DJ_RATE, (jcut_at + jcut_len) * 1000):]]),
        cut=(jcut_at, jcut_len))
    # A bar of the intro played twice: the bar before 22 s again at 22 s,
    # everything from there on a bar later.
    jins_at = 22.0
    ins = lead(DJ_RATE, jins_at * 1000)
    bar = lead(DJ_RATE, BAR_S * 1000)
    jins = add("j-insert.flac", jsrc, DJ_RATE, "flac", 0,
               np.concatenate([jbase[:ins], jbase[ins - bar:ins], jbase[ins:]]))
    jins.insert = (jins_at, BAR_S)

    # (k) The loop all through, 135 s of it: every window torn.
    ksrc = Source("k", [("kintro", 20.3, Event(np.random.default_rng(1201), 0.6))])
    kbase = LOOP_LEVEL * render_loop(J_S, vary_seed=1202, spread=0.03, ghosts=False, kit=TONAL_KIT, rate=DJ_RATE) \
        + ksrc.render(DJ_RATE, 0.0, J_S)
    add("k-ref.flac", ksrc, DJ_RATE, "flac", 0, kbase)
    n = lead(DJ_RATE, 21)
    add("k-lead21.flac", ksrc, DJ_RATE, "flac", n, with_lead(DJ_RATE, kbase, n))
    return files


def measure(files, out, dump):
    """Decode every file and find its markers; read every MP3's lead-in."""
    for f in files.values():
        path = os.path.join(out, f.name)
        rate, samples = decode(path)
        assert rate == f.rate, "%s decodes at %d, not %d" % (f.name, rate, f.rate)
        lossless = f.kind in ("wav", "flac")
        residuals = []
        for eid, onset, event in f.source.events:
            want = f.constructed(onset)
            if want is None:
                continue
            # Encoded copies may sit up to an encoder's delay later.
            at, norm = locate(samples, rate, event, want + (0.0 if lossless else 0.03),
                              search_s=0.006 if lossless else 0.06)
            # Over a loop a burst is not alone in its window, and its
            # correlation is lower; a lossless copy's position is checked
            # against the construction below all the same.
            assert at is not None and norm > (0.2 if lossless else 0.6), "%s: %s not found (%.3f)" % (
                f.name, eid, norm)
            f.events[eid] = at
            residuals.append((at - want) * 1000.0)
        if residuals:
            spread = max(residuals) - min(residuals)
            if lossless:
                assert max(abs(x) for x in residuals) < 0.01, "%s: a lossless copy measured off %s" % (f.name, residuals)
            else:
                assert spread < 0.1, "%s: markers disagree by %.3f ms: %s" % (f.name, spread, residuals)
            f.residual_ms = 0.0 if lossless else sum(residuals) / len(residuals)
        if f.kind.startswith("mp3"):
            f.mp3_class, f.offset_ms = lead_in(dump, path)


# Pairs: (id, shard, a, b, truth, also, why). truth is "construct"
# (computed from the two files, and checked against the markers), a
# number, or "unknown". `also` lists other answers accepted, each a
# number or "unknown".
PAIRS = [
    ("lead21", "lossless", "a-ref.flac", "a-lead21.flac", "construct", [],
     "926 samples (21 ms) of silence put in front"),
    ("lead46", "lossless", "a-ref.flac", "a-lead46.flac", "construct", [],
     "2029 samples (46 ms) of silence put in front"),
    ("lead160-wav", "lossless", "a-ref.wav", "a-lead160.wav", "construct", [],
     "160 ms of silence in front, WAV at 11.025 kHz"),
    ("lead675", "lossless", "a-ref.flac", "a-lead675.flac", "construct", [],
     "675 ms of silence in front"),
    ("lead1300", "lossless", "a-ref.flac", "a-lead1300.flac", "construct", [],
     "1.3 s of silence in front"),
    ("trim300", "lossless", "a-ref.flac", "a-trim300.flac", "construct", [],
     "the first 300 ms taken off"),
    ("tail", "lossless", "a-ref.flac", "a-tail.flac", "construct", [],
     "2 s of other bursts added at the end, the start untouched"),
    ("gain6", "lossless", "a-ref.flac", "a-gain6.flac", "construct", [],
     "6 dB quieter, nothing moved"),
    ("rate48k", "lossless", "a-ref.flac", "a-48k-lead120.flac", "construct", [],
     "the same signal sampled at 48 kHz, 120 ms of silence in front"),
    ("invert", "lossless", "a-ref.flac", "a-invert.flac", "construct", ["unknown"],
     "polarity inverted: every sound is where it was, so 0 is right; a probe that "
     "looks only for positive correlation may say unknown, which moves no cue"),
    ("other", "lossless", "a-ref.flac", "a-other.flac", "unknown", [],
     "a different signal of the same length: no shift lines them up"),
    ("cut-short", "lossless", "a-ref.flac", "a-cut.flac", "unknown", [],
     "400 ms cut out at 1.6 s of 4.5 s: sounds before it are 0 apart, after it "
     "400 ms; no one shift is right, and the file is too short for separate windows"),
    ("mp3B", "codec", "a-ref.flac", "a-ref-B.mp3", "construct", [],
     "ffmpeg's MP3 (Lavc Info frame, class B) of the same samples: what the decoder adds or trims"),
    ("mp3A", "codec", "a-ref.flac", "a-lead46-A.mp3", "construct", [],
     "libmp3lame without an Info frame (class A), 46 ms in front: the encoder delay is not trimmed"),
    ("mp3B-mp3D", "codec", "a-ref-B.mp3", "a-lead21-D.mp3", "construct", [],
     "class B against libmp3lame with a LAME tag (class D), 21 ms in front"),
    ("mp3A-m4a", "codec", "a-lead46-A.mp3", "a-ref.m4a", "construct", [],
     "class A MP3 against AAC in MP4"),
    ("m4a-m4a", "codec", "a-ref.m4a", "a-lead160.m4a", "construct", [],
     "two AAC copies, 160 ms apart"),
    ("mp3D-m4a", "codec", "a-lead21-D.mp3", "a-lead160.m4a", "construct", [],
     "class D MP3 against AAC, 139 ms apart before the codecs"),
    ("wav-mp3B", "codec", "a-lead160.wav", "a-ref-B.mp3", "construct", [],
     "11.025 kHz WAV against a 44.1 kHz MP3"),
    ("pure-730", "loop", "b-ref.flac", "b-lead730.flac", "construct", ["unknown"],
     "an exactly repeating loop, 730 ms of silence in front (not a multiple of its "
     "bar or beat): only the silence tells the true shift, so unknown is accepted too"),
    ("pure-bar", "loop", "b-ref.flac", "b-bar.flac", "unknown", ["0", "2000"],
     "an exactly repeating loop with one more bar in front: b-bar from 0 s is b-ref "
     "sample for sample, and from 2 s on too. 2000 ms is how it was made and 0 puts "
     "every cue on the same samples, so both are right, and unknown is accepted"),
    ("near-eq-1300", "loop", "n-ref.flac", "n-eq-1300.flac", "construct", ["unknown"],
     "the loop with 1% of its energy not repeating, equal lengths, the second copy "
     "starting 1.3 s further in (the review's case): the true shift is -1300 ms; "
     "a bar either side correlates at 0.99 of it, so unknown is accepted too"),
    ("real-620", "loop", "c-ref.flac", "c-lead620.flac", "construct", [],
     "a varied loop with bursts and a pad, 620 ms in front (more than a beat)"),
    ("real-1300", "loop", "c-ref.flac", "c-lead1300.flac", "construct", [],
     "the same, 1.3 s in front (more than half a bar)"),
    ("real-eq-1300", "loop", "c-ref.flac", "c-eq-1300.flac", "construct", [],
     "the same loop, equal lengths, the second copy starting 1.3 s further in"),
    ("intro675", "music", "d-ref.flac", "d-intro675.flac", "construct", [],
     "music after a 3 s silent intro, the intro 675 ms longer"),
    ("intro675-m4a", "music", "d-ref.flac", "d-intro675.m4a", "construct", [],
     "the same, the longer copy as AAC"),
    # The long pairs one to a shard, so that each test fits a CI runner
    # thirty times slower than a desktop under ctest's 300 s.
    ("long-tail", "long-tail", "L-ref.flac", "L-tail.flac", "construct", [],
     "85 s, 6 s of other bursts added at the end: the end window lines up at 0 too"),
    ("long-cut", "long-cut", "L-ref.flac", "L-cut.flac", "unknown", [],
     "85 s with 400 ms cut out at 42.5 s: the start window says 0, the end -400"),
    ("dj-21", "dj-21", "j-ref.flac", "j-lead21.flac", "construct", [],
     "135 s, a loop intro and outro around a middle that does not repeat, 21 ms in front: the start and "
     "end windows have a rival a bar away at 0.99 of the true lag, the middle settles them"),
    ("dj-1300", "dj-1300", "j-ref.flac", "j-lead1300.flac", "construct", [],
     "the same, 1.3 s in front: more than a beat, the start and end torn the same way"),
    ("dj-barcut", "dj-barcut", "j-ref.flac", "j-barcut.flac", "unknown", [],
     "the same with a whole bar cut from the middle of one copy: the start says 0, the end -2000"),
    ("dj-loop", "dj-loop", "k-ref.flac", "k-lead21.flac", "unknown", [],
     "135 s of the loop all through, 21 ms in front: every window has a rival at 0.99, none settles"),
    ("dj-insert", "dj-insert", "j-ref.flac", "j-insert.flac", "unknown", [],
     "a bar of the loop intro played twice at 22 s in one copy: the middle and end say +2000, the start "
     "window's own best is 0 (a bar away scores 0.9998 of it). Unknown, because every window's own best "
     "must agree; see README"),
]


def pair_truth(files, pair):
    pid, _, an, bn, truth, _, _ = pair
    a, b = files[an], files[bn]
    if truth == "unknown":
        return None
    if truth != "construct":
        return float(truth)
    assert a.source is b.source and a.cut is None and b.cut is None, pid
    constructed = (b.offset_s - a.offset_s) * 1000.0 + b.residual_ms - a.residual_ms
    common = sorted(set(a.events) & set(b.events))
    if common:
        diffs = [(b.events[k] - a.events[k]) * 1000.0 for k in common]
        assert max(abs(d - constructed) for d in diffs) < 0.1, "%s: markers %s, constructed %.4f" % (
            pid, diffs, constructed)
    return constructed


def fmt(x):
    return "%.4f" % x


def write_manifests(files, out):
    with open(os.path.join(out, "files.tsv"), "w") as tsv:
        tsv.write("# file\trate\tformat\tseconds\tlead_in_class\tcue_timeline_offset_ms\tcodec_ms\tmarkers\n")
        for f in sorted(files.values(), key=lambda f: f.name):
            markers = ",".join("%s:%s" % (k, fmt(v * 1000.0)) for k, v in sorted(f.events.items(), key=lambda kv: kv[1]))
            tsv.write("\t".join([f.name, str(f.rate), f.kind, "%.4f" % (len(f.samples) / f.rate), f.mp3_class,
                                 fmt(f.offset_ms), fmt(f.residual_ms), markers or "-"]) + "\n")
    with open(os.path.join(out, "pairs.tsv"), "w") as tsv:
        tsv.write("# id\tshard\ta\tb\tshift_ms\talso_accepted\twhy\n")
        for pair in PAIRS:
            pid, shard, an, bn, _, also, why = pair
            truth = pair_truth(files, pair)
            tsv.write("\t".join([pid, shard, an, bn, "unknown" if truth is None else fmt(truth),
                                 "|".join(also) if also else "-", why]) + "\n")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    dump = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "tests", "fixtures", "alignment")
    os.makedirs(out, exist_ok=True)
    for name in os.listdir(out):
        if os.path.splitext(name)[1] in (".flac", ".wav", ".mp3", ".m4a", ".tsv"):
            os.remove(os.path.join(out, name))
    files = build(out)
    measure(files, out, dump)
    write_manifests(files, out)
    total = 0
    for name in sorted(os.listdir(out)):
        size = os.path.getsize(os.path.join(out, name))
        total += size
        print("%8d  %s" % (size, name))
    print("%8d  in all" % total)


if __name__ == "__main__":
    main()
