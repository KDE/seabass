<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
-->

# Spike: a track as a ring around its cover art

Not built, not shipped, not in the test suite. It exists to judge a look.

`ring.frag` draws the whole thing; `TrackRing.qml` feeds it the waveform
as an N x 1 texture and the art as another. `spike_ring.qml` scans a
library, picks tracks and saves a sheet of rings.

    /usr/lib/qt6/bin/qsb --glsl "100 es,120,150" --hlsl 50 --msl 12 -o ring.frag.qsb ring.frag
    ./render.sh <build dir> <output dir>

`render.sh` draws it on the machine's real GPU inside a headless KWin of
its own (`kwin_wayland --virtual` under `dbus-run-session`), so nothing
appears on the desktop, and ends by naming the GPU that drew it.

It cannot use the `offscreen` platform the suite runs on: offscreen falls
back to the software scene graph, which draws no ShaderEffect at all --
the sheet comes out empty and nothing says why. xvfb with
`QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1` does draw it, on llvmpipe.

`libraryPath` in the harness points at a copy of a real stick's PIONEER
folder; the repository's fixture has no artwork.
