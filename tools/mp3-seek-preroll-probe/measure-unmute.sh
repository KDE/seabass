#!/bin/bash
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# Records what sink_probe or handover_probe plays, on a PipeWire null
# sink made for the run and removed after it (nothing is played on a real
# device: both probes refuse to start when no device matches the sink).
#
#   measure-unmute.sh PROBE_DIR FILE CUE_MS JUMPS GAP_MS MODE...
#
# MODE is one of sink_probe's (plain, tap, clock, warm, warmtap) or
# handover, which runs handover_probe and analyse_handover.py instead.
# PROBE_DIR holds the built probes; the recordings go there too.
# LEAD=<ms> is handed to sink_probe's clock and warm modes, DELAY=<ms> to
# handover_probe.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
dir=$1; file=$2; cue=$3; jumps=$4; gap=$5; shift 5
# The owner's default output stays the default: a new sink must not take
# over what other programs play.
default_sink=$(pactl get-default-sink)
module=$(pactl load-module module-null-sink sink_name=seabass_probe \
    sink_properties=device.description=SeabassProbe rate=48000)
trap 'pactl unload-module "$module"' EXIT
sleep 1
if [ "$(pactl get-default-sink)" != "$default_sink" ]; then
    pactl set-default-sink "$default_sink"
fi
for mode in "$@"; do
    pw-record -P stream.capture.sink=true --target seabass_probe --format f32 --rate 48000 \
        --channels 2 "$dir/rec-$mode.wav" &
    recorder=$!
    sleep 0.7
    if [ "$mode" = handover ]; then
        QT_QPA_PLATFORM=offscreen timeout 300 "$dir/handover_probe" "$file" "$cue" "$jumps" seabass_probe "$gap" \
            >"$dir/rec-$mode.log" 2>&1 || true
    else
        QT_QPA_PLATFORM=offscreen timeout 300 "$dir/sink_probe" "$file" "$cue" "$mode" "$jumps" seabass_probe "$gap" \
            >"$dir/rec-$mode.log" 2>&1 || true
    fi
    sleep 0.7
    kill -INT "$recorder"
    wait "$recorder" 2>/dev/null || true
    if [ "$mode" = handover ]; then
        echo "=== $mode DELAY=${DELAY:-0}"
        python3 "$here/analyse_handover.py" "$dir/rec-$mode.wav" | tail -1
    else
        echo "=== $mode LEAD=${LEAD:-0}"
        python3 "$here/analyse_unmute.py" "$dir/rec-$mode.wav" | tail -1
    fi
done
