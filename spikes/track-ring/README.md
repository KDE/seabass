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
    xvfb-run -a -s "-screen 0 1920x1200x24" env QT_QPA_PLATFORM=xcb QSG_RHI_BACKEND=opengl \
        LIBGL_ALWAYS_SOFTWARE=1 SEABASS_SCREENSHOT_DIR=<dir> \
        <build>/seabass_qml_tests -import $PWD -input $PWD/spike_ring.qml

It must run on xcb with software OpenGL, not the `offscreen` platform the
suite uses: offscreen falls back to the software scene graph, which draws
no ShaderEffect at all -- the sheet comes out empty and nothing says why.

`libraryPath` in the harness points at a copy of a real stick's PIONEER
folder; the repository's fixture has no artwork.
