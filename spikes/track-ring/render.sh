#!/bin/bash
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Renders the sheet of rings on the machine's real GPU without touching
# the desktop session: a headless KWin of its own, on its own D-Bus, is
# the display. usage: render.sh <build dir> <output dir>
set -e
BUILD=${1:?build dir holding seabass_qml_tests}
OUT=${2:?output dir}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
INNER=$(mktemp "$OUT/inner.XXXXXX.sh")
cat > "$INNER" <<INNER_EOF
#!/bin/bash
cd "$HERE"
exec env -u DISPLAY QT_QPA_PLATFORM=wayland QSG_RHI_BACKEND=opengl SEABASS_SCREENSHOT_DIR="$OUT" \
    "$BUILD/seabass_qml_tests" -import "$HERE" -input "$HERE/spike_ring.qml" > "$OUT/render.log" 2>&1
INNER_EOF
chmod +x "$INNER"
timeout 180 dbus-run-session -- kwin_wayland --virtual --socket "wl-seabass-ring-$$" \
    --width 1920 --height 1200 --no-lockscreen --no-global-shortcuts \
    --exit-with-session "$INNER" > "$OUT/kwin.log" 2>&1
rm -f "$INNER"
grep -E "Totals|FAIL|QWARN" "$OUT/render.log" || true
# Which GPU drew it: Qt names the driver in the pipeline cache it writes.
strings -n 6 ~/.cache/seabass_qml_tests/qtpipelinecache-*/qqpc_opengl 2>/dev/null | grep -m1 -iE "radeon|llvmpipe|intel|nvidia|mesa" || true
