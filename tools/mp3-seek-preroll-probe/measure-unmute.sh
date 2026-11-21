#!/bin/bash
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# Runs the #92 probes against a PipeWire null sink made for the run and
# removed after it, and records what they play. Safeguards against
# playing on, or recording from, anything else:
#
#   - it refuses to run unless the sound server is PipeWire and Qt will
#     use its FFmpeg media backend and PipeWire audio backend
#     (QT_MEDIA_BACKEND and QT_AUDIO_BACKEND unset or set to those; both
#     are set for the probes);
#   - the null sink is loaded with the lowest session priority, so the
#     session manager should not make it the default output. The default
#     is checked twice a second; if the null sink becomes it, the run
#     stops (the user's own streams would follow it and be recorded).
#     The user's configured default (PipeWire's default.configured.audio.sink
#     metadata) is read at the start and, if it changed during the run,
#     put back as it was (or removed again if there was none); the
#     effective default is never set directly;
#   - the probes play only on the device whose id they are given, and
#     refuse to start when there is none;
#   - while a probe runs, every playback stream of the probes (told by
#     node name) is checked twice a second; one on any other sink stops
#     the probe and the run. Up to half a second of sound could reach a
#     wrong sink before that check sees it. A run in which no probe
#     stream was ever seen on the null sink fails (the check could not
#     have worked);
#   - the recorder must be recording the null sink's monitor, or the run
#     stops before anything is played; it is started with
#     node.dont-reconnect, so it is not moved to another source if the
#     sink disappears, and if the sink disappears the recorder is stopped
#     first and the run fails;
#   - on any exit, Ctrl-C included, the probe is stopped first, then the
#     recorder, and the sink is unloaded last.
#
#   measure-unmute.sh PROBE_DIR FILE CUE_MS JUMPS GAP_MS MODE...
#   measure-unmute.sh --run PROBE_DIR COMMAND [ARG...]
#
# MODE is one of sink_probe's (plain, tap, clock, warm, warmtap, live),
# handover (handover_probe), switch (switch_probe, which takes the WAV to
# switch to from PCM=<path>) or zero (zero_probe; CUE_MS is its MARK_MS).
# For each mode the recording is PROBE_DIR/rec-<mode><TAG>.wav, the
# probe's output rec-<mode><TAG>.log and the analysis, one line per jump,
# rec-<mode><TAG>.txt; the analysis's summary line is printed. TAG=<text>
# in the environment keeps runs apart. LEAD=<ms> goes to sink_probe's
# clock and warm modes, FROM=<ms> to its live mode, DELAY=<ms> to
# handover_probe, SOURCE=buffer to sink_probe. With REF=<WAV from
# pcm_probe> the recording is read by analyse_real.py against that
# reference instead (jumps on real music, no click).
#
# --run runs COMMAND (with the null sink's device id as $PROBE_DEVICE)
# while the sink exists, without recording, and writes its output to
# PROBE_DIR/run<TAG>.log: for warm_cost.
set -euo pipefail

die() { echo "measure-unmute.sh: $*" >&2; exit 1; }

case "${QT_AUDIO_BACKEND:-pipewire}" in
    pipewire) ;;
    *) die "QT_AUDIO_BACKEND=$QT_AUDIO_BACKEND: only Qt's PipeWire backend pins a stream to its sink" ;;
esac
case "${QT_MEDIA_BACKEND:-ffmpeg}" in
    ffmpeg) ;;
    *) die "QT_MEDIA_BACKEND=$QT_MEDIA_BACKEND: the probes measure Qt's FFmpeg backend" ;;
esac
pactl info | grep -q '^Server Name: .*on PipeWire' || die "the sound server is not PipeWire"
for tool in pw-record pw-metadata python3; do
    command -v "$tool" >/dev/null || die "$tool not found"
done
export QT_AUDIO_BACKEND=pipewire
export QT_MEDIA_BACKEND=ffmpeg
export QT_QPA_PLATFORM=offscreen
export LC_ALL=C

here=$(cd "$(dirname "$0")" && pwd)
sink=seabass_probe
probe=""
recorder=""
module=""

# The configured default output, as PipeWire's metadata holds it (a JSON
# value, or nothing when the user never chose one).
configured_default() {
    pw-metadata -n default 0 default.configured.audio.sink 2>/dev/null \
        | sed -n "s/.*key:'default.configured.audio.sink' value:'\\([^']*\\)'.*/\\1/p"
}
configured_before=$(configured_default)

cleanup() {
    trap - EXIT INT TERM HUP
    if [ -n "$probe" ]; then kill "$probe" 2>/dev/null || true; wait "$probe" 2>/dev/null || true; fi
    if [ -n "$recorder" ]; then kill -INT "$recorder" 2>/dev/null || true; wait "$recorder" 2>/dev/null || true; fi
    if [ -n "$module" ]; then pactl unload-module "$module" || true; fi
    if [ "$(configured_default)" != "$configured_before" ]; then
        if [ -n "$configured_before" ]; then
            pw-metadata -n default 0 default.configured.audio.sink "$configured_before" Spa:String:JSON >/dev/null || true
        else
            pw-metadata -n default -d 0 default.configured.audio.sink >/dev/null || true
        fi
        echo "measure-unmute.sh: the configured default output changed during the run; put back as it was" >&2
    fi
}
trap cleanup EXIT
trap 'echo "measure-unmute.sh: interrupted" >&2; exit 130' INT TERM HUP

sink_exists() { pactl list short sinks | cut -f2 | grep -qx "$sink"; }

# Stops the run when the null sink has become the default output, or is gone.
check_sink() {
    if [ "$(pactl get-default-sink)" = "$sink" ]; then
        die "the null sink became the default output: stopped"
    fi
    if ! sink_exists; then
        if [ -n "$recorder" ]; then kill -INT "$recorder" 2>/dev/null || true; wait "$recorder" 2>/dev/null || true; recorder=""; fi
        die "the null sink disappeared: stopped"
    fi
}

# Prints the sink of every playback stream a probe has open, one a line.
# The streams are told by their node name, which Qt takes from the
# program's name (PipeWire gives them no process id here).
probe_names="sink_probe handover_probe switch_probe warm_cost zero_probe"
probe_sinks() {
    pactl -f json list sink-inputs | python3 -c '
import json, sys
probes = sys.argv[1].split()
names = {s["index"]: s["name"] for s in json.loads(sys.argv[2])}
for i in json.load(sys.stdin):
    if i.get("properties", {}).get("node.name") in probes:
        print(names.get(i["sink"], "?"))' "$probe_names" "$(pactl -f json list short sinks)"
}

# The name of the source the recorder records from, or nothing yet.
recorder_name=seabass-probe-recorder
recorder_source() {
    pactl -f json list source-outputs | python3 -c '
import json, sys
names = {s["index"]: s["name"] for s in json.loads(sys.argv[2])}
for o in json.load(sys.stdin):
    if o.get("properties", {}).get("node.name") == sys.argv[1]:
        print(names.get(o["source"], "?"))' "$recorder_name" "$(pactl -f json list short sources)"
}

# Runs the probe in the background and watches where it plays until it
# ends; its exit status is the function's. 300 s at most.
run_probe() {
    local log=$1; shift
    "$@" >"$log" 2>&1 &
    probe=$!
    local ticks=0 status=0 where all unresolved=0 seen=0
    while kill -0 "$probe" 2>/dev/null; do
        check_sink
        all=$(probe_sinks)
        if printf '%s\n' "$all" | grep -qx "$sink"; then
            seen=1
        fi
        # A stream that is still connecting has no sink yet ("?"): only
        # one that stays unresolved for three checks (1.5 s) counts.
        where=$(printf '%s\n' "$all" | grep -v '^$' | grep -vx "$sink" || true)
        if [ "$where" = "?" ] && [ "$unresolved" -lt 3 ]; then
            unresolved=$((unresolved + 1))
            where=""
        elif [ -z "$where" ]; then
            unresolved=0
        fi
        if [ -n "$where" ]; then
            kill "$probe"
            die "the probe played on $where, not on $sink: stopped"
        fi
        ticks=$((ticks + 1))
        if [ "$ticks" -gt 600 ]; then
            kill "$probe"
            die "the probe ran over 300 s: stopped, its data is incomplete"
        fi
        sleep 0.5
    done
    wait "$probe" || status=$?
    probe=""
    if [ "$status" -eq 0 ] && [ "$seen" -eq 0 ]; then
        die "no probe stream was ever seen on $sink: the routing check did not work, the run does not count"
    fi
    return "$status"
}

load_sink() {
    module=$(pactl load-module module-null-sink sink_name="$sink" \
        sink_properties="device.description=SeabassProbe priority.session=0 priority.driver=0" rate=48000)
    sleep 1
    check_sink
}

if [ "${1:-}" = "--run" ]; then
    shift
    dir=$1; shift
    load_sink
    export PROBE_DEVICE=$sink
    status=0
    run_probe "$dir/run${TAG:-}.log" "$@" || status=$?
    cat "$dir/run${TAG:-}.log"
    check_sink
    [ "$status" -eq 0 ] || die "$1 failed (exit $status)"
    exit 0
fi

dir=$1; file=$2; cue=$3; jumps=$4; gap=$5; shift 5
load_sink
for mode in "$@"; do
    base="$dir/rec-$mode${TAG:-}"
    pw-record -P "{ stream.capture.sink=true node.name=$recorder_name node.dont-reconnect=true }" \
        --target "$sink" --format f32 --rate 48000 --channels 2 "$base.wav" &
    recorder=$!
    sleep 0.7
    recording_from=$(recorder_source)
    if [ "$recording_from" != "$sink.monitor" ] && [ "$recording_from" != "$sink" ]; then
        die "pw-record is recording \"$recording_from\", not $sink's monitor: stopped before playing anything"
    fi
    case "$mode" in
        handover) cmd=("$dir/handover_probe" "$file" "$cue" "$jumps" "$sink" "$gap") ;;
        switch) cmd=("$dir/switch_probe" "$file" "${PCM:?PCM=<the WAV to switch to>}" "$jumps" "$sink" "$gap") ;;
        zero) cmd=("$dir/zero_probe" "$file" "$cue" "$jumps" "$sink" "$gap") ;;
        *) cmd=("$dir/sink_probe" "$file" "$cue" "$mode" "$jumps" "$sink" "$gap") ;;
    esac
    status=0
    run_probe "$base.log" "${cmd[@]}" || status=$?
    sleep 0.7
    check_sink
    kill -INT "$recorder"
    wait "$recorder" || true
    recorder=""
    if [ "$status" -ne 0 ]; then
        tail -5 "$base.log" >&2
        die "$mode: the probe failed (exit $status), no analysis"
    fi
    if [ -n "${REF:-}" ]; then
        python3 "$here/analyse_real.py" "$base.wav" "$REF" "$cue" >"$base.txt"
    else
        case "$mode" in
            handover|switch) analyse=analyse_handover.py ;;
            zero) analyse=analyse_zero.py ;;
            *) analyse=analyse_unmute.py ;;
        esac
        python3 "$here/$analyse" "$base.wav" >"$base.txt"
    fi
    echo "=== $mode${TAG:+ ($TAG)} LEAD=${LEAD:-0} DELAY=${DELAY:-0}"
    tail -1 "$base.txt"
done
