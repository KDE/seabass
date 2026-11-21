#!/bin/bash
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# Runs the #92 probes against a PipeWire null sink made for the run and
# removed after it, and records what they play. Nothing is played on a
# real output:
#
#   - it refuses to run unless the sound server is PipeWire and Qt will
#     use its PipeWire backend (QT_AUDIO_BACKEND unset or "pipewire"; it
#     is set to "pipewire" for the probes);
#   - the null sink is loaded with the lowest session priority, so the
#     session manager does not make it the default output; if it does
#     anyway, the default is put back (and only then);
#   - the probes play only on the device whose id they are given, and
#     refuse to start when there is none;
#   - while a probe runs, every playback stream of the probes (told by
#     node name) is checked twice a second; one on any other sink stops
#     the probe and the run;
#   - the recorder must be recording the null sink's monitor, or the run
#     stops before anything is played;
#   - on any exit, Ctrl-C included, the probe is stopped first, then the
#     recorder, and the sink is unloaded last.
#
#   measure-unmute.sh PROBE_DIR FILE CUE_MS JUMPS GAP_MS MODE...
#   measure-unmute.sh --run PROBE_DIR COMMAND [ARG...]
#
# MODE is one of sink_probe's (plain, tap, clock, warm, warmtap),
# handover (handover_probe) or switch (switch_probe, which takes the WAV
# to switch to from PCM=<path>). For each mode the recording is
# PROBE_DIR/rec-<mode><TAG>.wav, the probe's output rec-<mode><TAG>.log
# and the analysis, one line per jump, rec-<mode><TAG>.txt; the
# analysis's summary line is printed. TAG=<text> in the environment keeps
# runs apart. LEAD=<ms> goes to sink_probe's clock and warm modes,
# DELAY=<ms> to handover_probe, SOURCE=buffer to sink_probe. With
# REF=<WAV from pcm_probe> the recording is read by analyse_real.py
# against that reference instead (jumps on real music, no click).
#
# --run runs COMMAND (with the null sink's device id as $PROBE_DEVICE)
# while the sink exists, without recording: for warm_cost.
set -euo pipefail

die() { echo "measure-unmute.sh: $*" >&2; exit 1; }

case "${QT_AUDIO_BACKEND:-pipewire}" in
    pipewire) ;;
    *) die "QT_AUDIO_BACKEND=$QT_AUDIO_BACKEND: only Qt's PipeWire backend pins a stream to its sink" ;;
esac
pactl info | grep -q '^Server Name: .*on PipeWire' || die "the sound server is not PipeWire"
command -v pw-record >/dev/null || die "pw-record not found"
export QT_AUDIO_BACKEND=pipewire
export QT_QPA_PLATFORM=offscreen
export LC_ALL=C

here=$(cd "$(dirname "$0")" && pwd)
sink=seabass_probe
probe=""
recorder=""
module=""
default_sink=$(pactl get-default-sink)

cleanup() {
    trap - EXIT INT TERM HUP
    if [ -n "$probe" ]; then kill "$probe" 2>/dev/null || true; wait "$probe" 2>/dev/null || true; fi
    if [ -n "$recorder" ]; then kill -INT "$recorder" 2>/dev/null || true; wait "$recorder" 2>/dev/null || true; fi
    if [ -n "$module" ]; then pactl unload-module "$module" || true; fi
    if [ "$(pactl get-default-sink)" = "$sink" ]; then pactl set-default-sink "$default_sink" || true; fi
}
trap cleanup EXIT
trap 'echo "measure-unmute.sh: interrupted" >&2; exit 130' INT TERM HUP

# Fails unless the default is still the user's own.
check_default() {
    local now
    now=$(pactl get-default-sink)
    if [ "$now" = "$sink" ]; then
        pactl set-default-sink "$default_sink"
        echo "measure-unmute.sh: the null sink had become the default output; put $default_sink back" >&2
    fi
}

# Prints the sink of every playback stream a probe has open, one a line.
# The streams are told by their node name, which Qt takes from the
# program's name (PipeWire gives them no process id here).
probe_names="sink_probe handover_probe switch_probe warm_cost"
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
    local ticks=0 status=0 where unresolved=0
    while kill -0 "$probe" 2>/dev/null; do
        # A stream that is still connecting has no sink yet ("?"): only
        # one that stays unresolved for a second and a half counts.
        where=$(probe_sinks | grep -vx "$sink" || true)
        if [ "$where" = "?" ] && [ "$unresolved" -lt 2 ]; then
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
    return "$status"
}

load_sink() {
    module=$(pactl load-module module-null-sink sink_name="$sink" \
        sink_properties="device.description=SeabassProbe priority.session=0 priority.driver=0" rate=48000)
    sleep 1
    check_default
}

if [ "${1:-}" = "--run" ]; then
    shift
    dir=$1; shift
    load_sink
    export PROBE_DEVICE=$sink
    status=0
    run_probe "$dir/run.log" "$@" || status=$?
    cat "$dir/run.log"
    check_default
    [ "$status" -eq 0 ] || die "$1 failed (exit $status)"
    exit 0
fi

dir=$1; file=$2; cue=$3; jumps=$4; gap=$5; shift 5
load_sink
for mode in "$@"; do
    base="$dir/rec-$mode${TAG:-}"
    pw-record -P "{ stream.capture.sink=true node.name=$recorder_name }" --target "$sink" --format f32 --rate 48000 \
        --channels 2 "$base.wav" &
    recorder=$!
    sleep 0.7
    recording_from=$(recorder_source)
    if [ "$recording_from" != "$sink.monitor" ] && [ "$recording_from" != "$sink" ]; then
        die "pw-record is recording \"$recording_from\", not $sink's monitor: stopped before playing anything"
    fi
    case "$mode" in
        handover) cmd=("$dir/handover_probe" "$file" "$cue" "$jumps" "$sink" "$gap") ;;
        switch) cmd=("$dir/switch_probe" "$file" "${PCM:?PCM=<the WAV to switch to>}" "$jumps" "$sink" "$gap") ;;
        *) cmd=("$dir/sink_probe" "$file" "$cue" "$mode" "$jumps" "$sink" "$gap") ;;
    esac
    status=0
    run_probe "$base.log" "${cmd[@]}" || status=$?
    sleep 0.7
    kill -INT "$recorder"
    wait "$recorder" || true
    recorder=""
    check_default
    if [ "$status" -ne 0 ]; then
        tail -5 "$base.log" >&2
        die "$mode: the probe failed (exit $status), no analysis"
    fi
    if [ -n "${REF:-}" ]; then
        python3 "$here/analyse_real.py" "$base.wav" "$REF" "$cue" >"$base.txt"
    else
        case "$mode" in
            handover|switch) analyse=analyse_handover.py ;;
            *) analyse=analyse_unmute.py ;;
        esac
        python3 "$here/$analyse" "$base.wav" >"$base.txt"
    fi
    echo "=== $mode${TAG:+ ($TAG)} LEAD=${LEAD:-0} DELAY=${DELAY:-0}"
    tail -1 "$base.txt"
done
