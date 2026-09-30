#!/bin/bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# tools/macos-bundle-check.sh, run on Linux. No otool here, so a shim on
# PATH prints canned `otool -l` output for each fake Mach-O file (a Mach-O
# header and nothing else, which file(1) recognises as such). Every case
# asserts the message it expects, not just the exit status, so a check that
# refuses for some other reason does not pass for the one under test.
#
# SEABASS_BUNDLE_CHECK names the file to test; tools/macos-bundle-check.sh
# beside this one by default. Pointing it at a deliberately broken copy is
# how each case was seen to go red.
set -u
set -o pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
check="${SEABASS_BUNDLE_CHECK:-$here/../tools/macos-bundle-check.sh}"
[ -f "$check" ] || { echo "no such file: $check" >&2; exit 1; }
command -v file >/dev/null 2>&1 || { echo "file(1) is not on PATH, and the check needs it" >&2; exit 1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/seabass-bundle-check.XXXXXX")" || exit 1
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/bin" "$work/canned"
cat > "$work/bin/otool" <<'EOF'
#!/bin/bash
# otool -l <file>: the canned load commands for that file's name
[ "$1" = "-l" ] || { echo "shim: only -l" >&2; exit 2; }
canned="$OTOOL_CANNED/$(basename "$2")"
[ -f "$canned" ] || { echo "shim: nothing canned for $2" >&2; exit 1; }
cat "$canned"
EOF
chmod +x "$work/bin/otool"
export PATH="$work/bin:$PATH" OTOOL_CANNED="$work/canned"
# The floor at 1 MB, so the fixtures stay small; the fixture carries 2 MB.
export SEABASS_BUNDLE_MIN_MB=1

macho() {  # <path>: a 64-bit arm64 Mach-O header, enough for file(1)
    mkdir -p "$(dirname "$1")"
    printf '\xcf\xfa\xed\xfe\x0c\x00\x00\x01\x00\x00\x00\x00\x06\x00\x00\x00' > "$1"
}
n=0
lc() {  # <cmd> <path>: one load command as otool -l prints it
    local field=name
    [ "$1" = LC_RPATH ] && field=path
    printf 'Load command %d\n          cmd %s\n      cmdsize 56\n         %s %s (offset 24)\n' "$n" "$1" "$field" "$2"
    [ "$field" = name ] && printf '   time stamp 2 Thu Jan  1 01:00:02 1970\n      current version 6.8.0\ncompatibility version 6.0.0\n'
    n=$((n + 1))
}
segment() { printf 'Load command %d\n      cmd LC_SEGMENT_64\n  cmdsize 72\n  segname __PAGEZERO\n' "$n"; n=$((n + 1)); }

# A bundle shaped like a deployed one: an executable, Qt as a framework,
# an FFmpeg dylib and a Qt plugin, each loading through @rpath or the
# system. The executable's output is a fat file's, one block per slice.
make_bundle() {  # <app>
    local app="$1"
    rm -rf "$app" "$work/canned"/*
    macho "$app/Contents/MacOS/seabass-cli"
    macho "$app/Contents/Frameworks/QtCore.framework/Versions/A/QtCore"
    ln -s Versions/A/QtCore "$app/Contents/Frameworks/QtCore.framework/QtCore"
    macho "$app/Contents/Frameworks/libavcodec.62.dylib"
    macho "$app/Contents/PlugIns/platforms/libqcocoa.dylib"
    macho "$app/Contents/Frameworks/libwebp.7.2.0.dylib"
    ln -s libwebp.7.2.0.dylib "$app/Contents/Frameworks/libwebp.7.dylib"
    macho "$app/Contents/Frameworks/libsharpyuv.0.dylib"
    macho "$app/Contents/Frameworks/libwebpdemux.2.0.17.dylib"
    mkdir -p "$app/Contents/Resources"
    head -c 2097152 /dev/zero > "$app/Contents/Resources/filler.qm"
    printf 'plist\n' > "$app/Contents/Info.plist"
    {
        for arch in arm64 x86_64; do
            echo "$app/Contents/MacOS/seabass-cli (architecture $arch):"
            n=0; segment
            lc LC_LOAD_DYLIB @rpath/QtCore.framework/Versions/A/QtCore
            lc LC_LOAD_DYLIB @rpath/libavcodec.62.dylib
            lc LC_LOAD_DYLIB /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation
            lc LC_LOAD_DYLIB /usr/lib/libSystem.B.dylib
            lc LC_LOAD_WEAK_DYLIB @rpath/libOptional.dylib
            lc LC_RPATH @executable_path/../Frameworks
        done
    } > "$work/canned/seabass-cli"
    {
        echo "$app/Contents/Frameworks/QtCore.framework/Versions/A/QtCore:"
        n=0; segment
        lc LC_ID_DYLIB @rpath/QtCore.framework/Versions/A/QtCore
        lc LC_LOAD_DYLIB /usr/lib/libc++.1.dylib
        lc LC_RPATH @loader_path/../../..
    } > "$work/canned/QtCore"
    {
        echo "$app/Contents/Frameworks/libavcodec.62.dylib:"
        n=0; segment
        lc LC_ID_DYLIB @rpath/libavcodec.62.dylib
        lc LC_LOAD_DYLIB /usr/lib/libz.1.dylib
    } > "$work/canned/libavcodec.62.dylib"
    {
        echo "$app/Contents/PlugIns/platforms/libqcocoa.dylib:"
        n=0; segment
        lc LC_ID_DYLIB @rpath/libqcocoa.dylib
        lc LC_LOAD_DYLIB @rpath/QtCore.framework/Versions/A/QtCore
        lc LC_LOAD_DYLIB @loader_path/../../Frameworks/libavcodec.62.dylib
        lc LC_RPATH @loader_path/../../Frameworks
    } > "$work/canned/libqcocoa.dylib"
    # What KDE's cache build of libwebp really ships: its build directory
    # left behind as an LC_RPATH, while every load resolves in the bundle.
    # A warning, not a refusal.
    local webp_build=/Users/gitlab/builds/xyz/craft-ci/macos-arm-clang/build/libs/webp/work/build
    {
        echo "$app/Contents/Frameworks/libwebp.7.2.0.dylib:"
        n=0; segment
        lc LC_ID_DYLIB @rpath/libwebp.7.dylib
        lc LC_LOAD_DYLIB @executable_path/../Frameworks/libsharpyuv.0.dylib
        lc LC_LOAD_DYLIB /usr/lib/libSystem.B.dylib
        lc LC_RPATH "$webp_build"
    } > "$work/canned/libwebp.7.2.0.dylib"
    {
        echo "$app/Contents/Frameworks/libsharpyuv.0.dylib:"
        n=0; segment
        lc LC_ID_DYLIB @rpath/libsharpyuv.0.dylib
    } > "$work/canned/libsharpyuv.0.dylib"
    {
        echo "$app/Contents/Frameworks/libwebpdemux.2.0.17.dylib:"
        n=0; segment
        lc LC_ID_DYLIB @rpath/libwebpdemux.2.dylib
        lc LC_LOAD_DYLIB @rpath/libwebp.7.dylib
        lc LC_LOAD_DYLIB @executable_path/../Frameworks/libsharpyuv.0.dylib
        lc LC_RPATH "$webp_build"
        lc LC_RPATH @loader_path
        # The second slice of a fat file carries the same leftover.
        echo "$app/Contents/Frameworks/libwebpdemux.2.0.17.dylib (architecture x86_64):"
        n=0; segment
        lc LC_RPATH "$webp_build"
    } > "$work/canned/libwebpdemux.2.0.17.dylib"
}

add_to() {  # <canned name> <cmd> <path>: one more load command for that file
    n=90; lc "$2" "$3" >> "$work/canned/$1"
}

failures=0
# <name> <expected status> [message the check must print]
expect() {
    local name="$1" want="$2" message="${3:-}" out rc
    out="$( (
        # shellcheck source=../tools/macos-bundle-check.sh
        . "$check"
        seabass_bundle_self_contained "$app"
    ) 2>&1)"
    rc=$?
    if [ "$rc" -ne "$want" ]; then
        echo "FAIL: $name: status $rc, wanted $want"; failures=$((failures + 1))
    elif [ -n "$message" ] && ! printf '%s\n' "$out" | grep -qF -- "$message"; then
        echo "FAIL: $name: no line saying \"$message\""; failures=$((failures + 1))
    else
        echo "PASS: $name"; return 0
    fi
    printf '%s\n' "$out" | sed 's/^/    /'
}

app="$work/seabass.app"

make_bundle "$app"
expect "a deployed bundle passes" 0 "self-contained: 7 Mach-O files"
expect "libwebp's leftover build rpath is a warning, not a refusal" 0 \
    "warning: Contents/Frameworks/libwebp.7.2.0.dylib: LC_RPATH /Users/gitlab/builds/xyz/craft-ci/macos-arm-clang/build/libs/webp/work/build lies outside the bundle"
expect "each warning is counted once" 0 "2 warning(s)"

make_bundle "$app"
add_to libavcodec.62.dylib LC_LOAD_DYLIB /Users/x/Seabass/builds/craft/lib/libvpx.9.dylib
expect "a load from a Craft root is refused, naming file and command" 1 \
    "Contents/Frameworks/libavcodec.62.dylib: LC_LOAD_DYLIB /Users/x/Seabass/builds/craft/lib/libvpx.9.dylib lies outside the bundle"

# The Craft root exists here, as it does on the Mac that built the bundle,
# so the load resolves, but only outside.
make_bundle "$app"
mkdir -p "$work/craft/lib" && : > "$work/craft/lib/libvpx.9.dylib"
add_to seabass-cli LC_RPATH "$work/craft/lib"
add_to seabass-cli LC_LOAD_DYLIB @rpath/libvpx.9.dylib
expect "an @rpath load found only through a search path outside the bundle is refused" 1 \
    "Contents/MacOS/seabass-cli: LC_LOAD_DYLIB @rpath/libvpx.9.dylib is found only through $work/craft/lib, outside the bundle"

make_bundle "$app"
add_to libqcocoa.dylib LC_RPATH @loader_path/../../../../lib
add_to libqcocoa.dylib LC_LOAD_DYLIB @rpath/libvpx.9.dylib
expect "an @rpath load no search path inside the bundle finds is refused" 1 \
    "Contents/PlugIns/platforms/libqcocoa.dylib: LC_LOAD_DYLIB @rpath/libvpx.9.dylib is not found through any LC_RPATH inside the bundle"

# macOS's file(1) on a fat binary prints a line for the file and then one
# per slice, "<path> (for architecture x86_64):<tab>Mach-O ...", with no
# separator on those lines. Linux's does
# not, so a file shim adds the slice lines for seabass-cli.
make_bundle "$app"
real_file="$(command -v file)"
cat > "$work/bin/file" <<SHIM
#!/bin/bash
"$real_file" "\$@" | while IFS= read -r line; do
    p="\${line%%//*}"
    case "\$p" in
        */Contents/MacOS/seabass-cli)
            printf '%s// Mach-O universal binary with 2 architectures: [x86_64:Mach-O 64-bit executable x86_64] [arm64]\n' "\$p"
            printf '%s (for architecture x86_64):\tMach-O 64-bit executable x86_64\n' "\$p"
            printf '%s (for architecture arm64):\tMach-O 64-bit executable arm64\n' "\$p" ;;
        *) printf '%s\n' "\$line" ;;
    esac
done
SHIM
chmod +x "$work/bin/file"
expect "a fat binary's per-slice file lines name one file" 0 "self-contained: 7 Mach-O files"
add_to seabass-cli LC_LOAD_DYLIB /opt/homebrew/lib/libz.1.dylib
expect "and the fat binary's load commands are still read" 1 \
    "Contents/MacOS/seabass-cli: LC_LOAD_DYLIB /opt/homebrew/lib/libz.1.dylib lies outside the bundle"
rm -f "$work/bin/file"

make_bundle "$app"
add_to libavcodec.62.dylib LC_LOAD_DYLIB "$app/Contents/Frameworks/QtCore.framework/Versions/A/QtCore"
expect "an absolute path into the bundle itself is refused" 1 \
    "is an absolute path: it finds the bundle only where it was built"

make_bundle "$app"
rm -rf "$app/Contents/Frameworks/QtCore.framework"
expect "a bundle without QtCore.framework is refused" 1 \
    "Contents/Frameworks has no QtCore.framework"

make_bundle "$app"
rm -f "$app/Contents/Resources/filler.qm"
expect "a bundle under the size floor is refused as the stub" 1 \
    "looks like Craft's undeployed stub"

echo
if [ "$failures" -eq 0 ]; then echo "all cases pass"; exit 0; fi
echo "$failures case(s) failed"
exit 1
