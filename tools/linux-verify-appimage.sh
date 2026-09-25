#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Proves a Seabass AppImage reads an encrypted OneLibrary with the SQLCipher
# it carries, on a system that has none of its own.
#
#   tools/linux-verify-appimage.sh seabass-0.7.10_alpha_linux.AppImage
#
# Needs docker; builds its image from ubuntu:26.04 the first time. Exits
# non-zero on any fault.
#
# Why a container and not this machine: SQLCipher is dlopen()ed by bare
# name (see src/infrastructure/onelibrary/sqlcipher_dyn.cpp), so linuxdeploy
# cannot find it through ldd, and a developer's machine usually has a
# libsqlcipher of its own. There the AppImage would load the host's copy and
# pass with nothing bundled at all, then fail on a DJ's laptop at the first
# rekordbox stick. The image is ubuntu:26.04 plus the four libraries every
# desktop has and an AppImage leaves to the host on purpose (linuxdeploy's
# excludelist: the GL dispatch libraries and X11). Without them the CLI
# stops at libGLX before it gets anywhere near SQLCipher, which proves
# nothing. It has no Qt, no SQLite and no SQLCipher, and this checks that.
#
# Three checks, and the third is what makes the first two mean anything:
#   1. seabass-cli digest reads the fixture's encrypted exportLibrary.db and
#      gives the digest a trusted build gives. The CONTENT, not just a line:
#      a reader that opened nothing and printed an empty catalog would still
#      print a line.
#   2. LD_DEBUG says libsqlcipher was loaded from inside the AppImage.
#   3. With the bundled libsqlcipher deleted, the OneLibrary line is gone.
#      If it is not, checks 1 and 2 were not measuring the bundle.
# `scan --rekordbox` is no use for any of this: it reads export.pdb and
# never loads SQLCipher, so it passes with or without it.
#
# And one about starting at all: an AppImage on the old runtime needs
# libfuse2, which Ubuntu 24.04 and later do not install.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

appimage="${1:?the .AppImage to check}"
[ -f "$appimage" ] || { echo "no such file: $appimage" >&2; exit 2; }
appimage="$(cd "$(dirname "$appimage")" && pwd)/$(basename "$appimage")"
image="${SEABASS_VERIFY_IMAGE:-seabass-appimage-host:26.04}"
fixture="$root/tests/fixtures/anonymized_library"
# What seabass-cli digest says about that fixture's OneLibrary. Taken from a
# trusted build (2026-09-25, release-tooling at 2e1cd81e) reading the same
# committed file. It changes if the fixture or the digest's definition in
# application/catalog_digest does, and then this line changes with it.
expected="${SEABASS_EXPECTED_ONELIBRARY_DIGEST:-d8c5556ad1e4e18bbd08bcb89dc9dbadf112dbfc40aa52e637e2c7423161bd76}"

if ! docker image inspect "$image" >/dev/null 2>&1; then
    [ "$image" = "seabass-appimage-host:26.04" ] || { echo "no image $image" >&2; exit 2; }
    docker build -q -t "$image" - >/dev/null <<'DOCKER'
FROM ubuntu:26.04
RUN apt-get update && apt-get install -y --no-install-recommends libglx0 libopengl0 libegl1 libx11-6 \
    && rm -rf /var/lib/apt/lists/*
DOCKER
fi
# The whole point of the container: no SQLCipher of its own to fall back to.
if docker run --rm "$image" sh -c 'ldconfig -p | grep -q -i -E "sqlcipher|sqlite"'; then
    echo "$image has SQLite or SQLCipher of its own: it cannot tell a bundled copy from its own" >&2
    exit 2
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/seabass-appimage.XXXXXX")"
trap 'rm -rf "$work"' EXIT
cp "$appimage" "$work/seabass.AppImage"
chmod +x "$work/seabass.AppImage"

in_box() {  # <command...>, in the clean image, with the work dir and the fixture
    # As this user, not root: the extracted tree lands in $work, and step 3
    # and the cleanup delete from it on the host.
    docker run --rm --user "$(id -u):$(id -g)" -v "$work:/work" -v "$fixture:/fixture:ro" \
        -w /work "$image" "$@"
}

fail=0
say() { printf '%s\n' "$*"; }
bad() { say "  FAIL: $*"; fail=1; }

say "== extracting (no FUSE needed for this)"
in_box ./seabass.AppImage --appimage-extract >/dev/null
cli="squashfs-root/usr/bin/seabass-cli"
[ -x "$work/$cli" ] || { echo "no $cli in the AppImage" >&2; exit 1; }
bundled="$(cd "$work" && find squashfs-root -name 'libsqlcipher.so*' | sort)"
if [ -z "$bundled" ]; then
    bad "no libsqlcipher.so* anywhere in the AppImage"
else
    say "  bundled: $(printf '%s ' $bundled)"
fi

say "== 1. the encrypted OneLibrary, read by what the AppImage carries"
out="$(in_box "./$cli" digest --rekordbox /fixture/rekordbox 2>&1 || true)"
got="$(printf '%s\n' "$out" | sed -n 's/^onelibrary:[^\t]*\t//p')"
read_ok=0
if [ "$got" = "$expected" ]; then
    read_ok=1
    say "  digest matches: $got"
else
    bad "OneLibrary digest is '${got:-<no onelibrary line>}', expected $expected"
    printf '%s\n' "$out" | sed 's/^/      /' | head -12
fi

say "== 2. and from inside the AppImage, not from anywhere else"
from="$(in_box env LD_DEBUG=libs "./$cli" digest --rekordbox /fixture/rekordbox 2>&1 \
        | sed -n 's/.*calling init: \(.*libsqlcipher[^ ]*\).*/\1/p' | head -1 || true)"
case "$from" in
    /work/squashfs-root/*) say "  loaded $from" ;;
    "") bad "libsqlcipher was never loaded" ;;
    *) bad "libsqlcipher came from $from, outside the AppImage" ;;
esac

say "== 3. control: without the bundled copy, the OneLibrary line must go"
for lib in $bundled; do rm -f "$work/$lib"; done
out="$(in_box "./$cli" digest --rekordbox /fixture/rekordbox 2>&1 || true)"
if [ "$read_ok" -ne 1 ]; then
    # A control only means something next to a run that worked. Without
    # one, the line is "gone" for whatever stopped check 1, and reporting
    # that as a pass is exactly what this control is here to prevent.
    bad "cannot run the control: check 1 never read the OneLibrary"
elif printf '%s\n' "$out" | grep -q "^onelibrary:.*$expected"; then
    bad "still read the OneLibrary with the bundled SQLCipher removed: checks 1 and 2 did not measure the bundle"
elif ! printf '%s\n' "$out" | grep -q -i 'sqlcipher'; then
    bad "the OneLibrary line went, but not because of SQLCipher:"
    printf '%s\n' "$out" | sed 's/^/      /' | head -6
else
    say "  gone, and for the right reason: $(printf '%s\n' "$out" | grep -i -m1 sqlcipher)"
fi

say "== 4. starts unextracted on a system without libfuse2"
start="$(in_box ./seabass.AppImage --appimage-version 2>&1 || true)"
run="$(in_box ./seabass.AppImage --help 2>&1 || true)"
if printf '%s\n%s\n' "$start" "$run" | grep -q 'libfuse.so.2'; then
    bad "the AppImage runtime wants libfuse2, which current Ubuntu does not ship"
else
    # No /dev/fuse in a container, so it may still not mount. That is the
    # container, not the image: what this looks for is the one error a
    # stock Ubuntu 24.04 desktop would give too.
    say "  no libfuse2 dependency (runtime: $(printf '%s' "$start" | head -1))"
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "NOT verified."
    exit 1
fi
echo "Verified: the AppImage reads an encrypted OneLibrary with its own SQLCipher."
