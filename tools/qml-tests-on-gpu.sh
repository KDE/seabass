#!/bin/bash
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Runs QML tests on this machine's real GPU without touching the desktop
# session -- for screenshots of anything a shader draws. A headless KWin
# of its own, on its own D-Bus, is the display.
#
#   tools/qml-tests-on-gpu.sh <build dir> <output dir> [<tests>]
#
# <tests> is a tst_*.qml file or a directory of them, tests/qml-shader by
# default. Screenshots the tests save land in <output dir>, next to
# render.log. Ends by naming the GPU that drew them, read from the
# pipeline cache Qt writes: a software fallback draws the same picture
# and would otherwise go unnoticed.
#
# The ctest entry seabass_qml_shader_tests does not use this. It runs on
# xvfb with Mesa's software OpenGL, which needs no GPU and no KWin.
set -e
BUILD=$(cd "${1:?build dir holding seabass_qml_tests}" && pwd)
OUT=${2:?output dir}
REPO=$(cd "$(dirname "$0")/.." && pwd)
INPUT=${3:-$REPO/tests/qml-shader}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
INNER=$(mktemp "$OUT/inner.XXXXXX")
cat > "$INNER" <<INNER_EOF
#!/bin/bash
exec env -u DISPLAY QT_QPA_PLATFORM=wayland QSG_RHI_BACKEND=opengl SEABASS_SCREENSHOT_DIR="$OUT" \\
    XDG_CONFIG_HOME="$BUILD/qml-test-config" \\
    "$BUILD/seabass_qml_tests" -input "$INPUT" > "$OUT/render.log" 2>&1
INNER_EOF
chmod +x "$INNER"
timeout 600 dbus-run-session -- kwin_wayland --virtual --socket "wl-seabass-gpu-$$" \
    --width 1920 --height 1200 --no-lockscreen --no-global-shortcuts \
    --exit-with-session "$INNER" > "$OUT/kwin.log" 2>&1 || true
rm -f "$INNER"
grep -E "^Totals|^FAIL|^QWARN" "$OUT/render.log" || { echo "no test output -- see $OUT/kwin.log"; exit 1; }
strings -n 6 "$HOME"/.cache/seabass_qml_tests/qtpipelinecache-*/qqpc_opengl 2>/dev/null \
    | grep -m1 -iE "radeon|llvmpipe|intel|nvidia|mesa" || echo "could not tell which GPU drew this"
! grep -q "^FAIL" "$OUT/render.log"
