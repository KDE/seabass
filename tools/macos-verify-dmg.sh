#!/bin/bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Check a macOS .dmg against a real stick, once per architecture it carries.
#
#   tools/macos-verify-dmg.sh <dmg> <stick mount point> [expectations]
#
# [expectations] is a file in the shape of the fixture's
# tests/fixtures/anonymized_library/SET-EXPECTATIONS.txt ("key value" per
# line). Given one, every slice must report exactly its rekordbox.tracks,
# rekordbox.cues and onelibrary.tracks. Without one the slices are only
# compared with each other, and two slices that read the stick equally
# wrongly -- 0 tracks each, say -- agree and pass; that is the mode for a
# real stick, whose counts nobody wrote down.
#
# A package is built with -DSEABASS_TESTS=OFF, so a green build says it
# compiled and bundled and nothing else. This is the only evidence that the
# thing in the .dmg works, and "it launches" is not that evidence: launching
# proves Qt resolved. Reading a catalog proves the dependencies did --
# SQLCipher for OneLibrary above all, which is the one that fails quietly in
# a cross-built binary and would not be noticed until a user opened a stick.
#
# Read-only throughout: scan and sync --dry-run only, and the stick is
# compared before and after.
#
# SEABASS_VERIFY_ARCHES names the architectures every Mach-O in the bundle
# must carry, and nothing else: "x86_64 arm64" (the universal package)
# when unset, "x86_64" for the Monterey one (tools/macos-monterey-dmg.sh).
set -u
set -o pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || exit 1
. "$here/macos-bundle-check.sh" || { echo "cannot read $here/macos-bundle-check.sh" >&2; exit 1; }
# A slice that found its libraries through DYLD_LIBRARY_PATH or
# DYLD_FRAMEWORK_PATH (a Craft shell sets them) proves nothing about the
# package, so none of them reaches the runs below.
seabass_clear_dyld_env
dmg="${1:?the .dmg to check}"
stick="${2:?a stick mount point, e.g. /Volumes/VSTICKB}"
expectations="${3:-}"
# Trailing slashes come free from tab-completion and broke the -prune paths
# below, which then reported a perfectly good package as having written to
# the stick.
while [ "${stick%/}" != "$stick" ]; do stick="${stick%/}"; done
[ -f "$dmg" ] || { echo "no such file: $dmg" >&2; exit 1; }
[ -d "$stick" ] || { echo "not mounted: $stick" >&2; exit 1; }

# The pinned counts are read before anything runs, and every one must be
# there: a key missing from the file would otherwise pin nothing and pass.
expect_rb_tracks=""; expect_rb_cues=""; expect_one_tracks=""
if [ -n "$expectations" ]; then
    [ -f "$expectations" ] || { echo "no such expectations file: $expectations" >&2; exit 1; }
    expected() { awk -v k="$1" '$1 == k && $2 ~ /^[0-9]+$/ { print $2; exit }' "$expectations"; }
    expect_rb_tracks="$(expected rekordbox.tracks)"
    expect_rb_cues="$(expected rekordbox.cues)"
    expect_one_tracks="$(expected onelibrary.tracks)"
    if [ -z "$expect_rb_tracks" ] || [ -z "$expect_rb_cues" ] || [ -z "$expect_one_tracks" ]; then
        echo "$expectations lacks rekordbox.tracks, rekordbox.cues or onelibrary.tracks" >&2
        exit 1
    fi
    echo "== expected: rekordbox $expect_rb_tracks tracks, $expect_rb_cues cues; onelibrary $expect_one_tracks tracks"
fi

# Colour codes out and NULs out, before any line is matched. The escape is
# made by printf because macOS's sed does not read \x1b.
esc="$(printf '\033')"
plain() { tr -d '\000' < "$1" | sed "s/${esc}\[[0-9;]*m//g"; }
# The number on the line "<label>: N", the whole label and nothing else on
# it, so "tracks:" can never pick up "rekordbox tracks:" or a track title.
count_of() {  # <log> <label>
    plain "$1" | awk -v l="$2:" '{ sub(/^[ \t]+/, "") } index($0, l) == 1 {
        rest = substr($0, length(l) + 1); gsub(/[ \t]/, "", rest)
        if (rest ~ /^[0-9]+$/) { print rest; exit } }'
}
# <what> <got> <expected>: empty expected means nothing is pinned.
pinned() {
    [ -n "$3" ] || return 0
    if [ "$2" = "$3" ]; then echo "  $1: $2, as expected"; return 0; fi
    echo "  $1: ${2:-none} where $3 was expected" >&2
    return 1
}

# The stick must be able to answer the question. rekordbox and OneLibrary
# are ONE library in two formats, so sync has nothing to reconcile between
# them and never opens OneLibrary unless there is an Engine library to sync
# against -- and OneLibrary is the whole point of the exercise, being the
# SQLCipher one. A stick without Engine cannot prove SQLCipher works, and
# saying so is better than passing without having looked.
if [ ! -d "$stick/Engine Library/Database2" ]; then
    echo "$stick has no Engine Library/Database2." >&2
    echo "Without an Engine side, sync never opens OneLibrary, so SQLCipher goes unchecked." >&2
    echo "Use a stick carrying all three catalogs (PIONEER/, exportLibrary.db and Engine Library/)." >&2
    exit 1
fi
[ -d "$stick/PIONEER" ] || { echo "$stick has no PIONEER folder" >&2; exit 1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/seabass-verify.XXXXXX")"
mp="$work/mount"; mkdir -p "$mp"
cleanup() {
    # Detached before the temp tree goes, and complained about if it will
    # not: rm -rf over a live mount leaves the image attached and the next
    # run attaches a second copy of it.
    # Compared on the resolved path: mount(8) reports /private/var/... while
    # $mp is the /var/... symlink, so matching the literal string skipped the
    # detach and let rm -rf loose on a live mount.
    local real; real="$(cd "$mp" 2>/dev/null && pwd -P || echo "$mp")"
    if mount | grep -qF " $real "; then
        hdiutil detach "$real" -quiet 2>/dev/null || hdiutil detach "$real" -force -quiet 2>/dev/null || {
            echo "could not detach $real -- the image is still attached" >&2; return; }
    fi
    rm -rf "$work"
}
trap cleanup EXIT

echo "== $dmg"
shasum -a 256 "$dmg"
hdiutil attach -nobrowse -readonly -mountpoint "$mp" "$dmg" >/dev/null || { echo "could not attach" >&2; exit 1; }
src="$(find "$mp" -maxdepth 1 -name '*.app' | head -1)"
[ -n "$src" ] || { echo "no .app in the image" >&2; exit 1; }
cp -R "$src" "$work/" || exit 1
app="$work/$(basename "$src")"
xattr -dr com.apple.quarantine "$app" 2>/dev/null
cli="$app/Contents/MacOS/seabass-cli"

fail_early=0
# Sorted, so "arm64 x86_64" and lipo's "x86_64 arm64" compare equal.
sorted_arches() { tr ' ' '\n' | grep -v '^$' | sort | tr '\n' ' '; }
wanted="$(printf '%s' "${SEABASS_VERIFY_ARCHES:-x86_64 arm64}" | sorted_arches)"
echo "== architectures in the bundle (wanted in every binary: $wanted)"
total=0; matching=0; wrong=""
while IFS= read -r f; do
    archs="$(lipo -archs "$f" 2>/dev/null)" || continue
    [ -z "$archs" ] && continue
    total=$((total+1))
    if [ "$(printf '%s' "$archs" | sorted_arches)" = "$wanted" ]; then
        matching=$((matching+1))
    else
        wrong="$wrong\n  ${f#$app/} -> $archs"
    fi
done < <(find "$app" -type f)
echo "  $total Mach-O files, $matching carrying exactly $wanted"
if [ -n "$wrong" ]; then
    printf "%b\n" "$wrong" >&2
    # For the universal package, a binary missing a slice is the defect
    # this was written for; for a one-architecture one, a stray slice is
    # a binary that was not thinned.
    echo "  a binary without exactly these architectures is the defect this checks for" >&2
    fail_early=1
fi
[ "$total" -gt 0 ] || { echo "  no Mach-O files found in the bundle at all" >&2; exit 1; }
if [ ! -x "$cli" ]; then
    echo "  no seabass-cli in the bundle at Contents/MacOS/seabass-cli" >&2; exit 1
fi
# Before anything runs: a slice that starts proves only that it found its
# libraries on THIS Mac, and a Mac with a Craft root at the path a binary
# names finds Qt there. That is how a merge of two undeployed halves passed
# every check. What the binaries ask for is read instead.
echo "== the bundle is self-contained"
seabass_bundle_self_contained "$app" || fail_early=1
arches="$(lipo -archs "$cli" 2>/dev/null || true)"
if [ -z "$arches" ]; then
    echo "  seabass-cli is not a Mach-O binary, so there is nothing to check" >&2; exit 1
fi
echo "  seabass-cli: $arches"
# The package must carry what was asked for, and saying so is the entire
# point of this branch: without it an arm64-only .dmg -- the defect being
# fixed -- passed.
if [ "$(printf '%s' "$arches" | sorted_arches)" != "$wanted" ]; then
    echo "  seabass-cli carries $arches, not $wanted" >&2; fail_early=1
fi

# A fingerprint of the stick, so "read-only" is checked rather than claimed.
#
# The same shape as rig-shakedown.sh's stick_tree(), and for its reasons:
# Seabass/caches holds derived data (probed durations, catalog mirrors) that
# any read may rebuild and that carries nothing of the library, and a
# directory's mtime moves whenever anything inside it does, including inside
# those caches. Files are compared exactly. SQLite's -wal/-shm/-journal
# sidecars are allowed to come and go, because opening a WAL database makes
# them; anything else appearing is a read that wrote, and fails.
stick_tree() {
    find "$1" -mindepth 1 \
        -path "$1/Seabass/caches" -prune -o \
        -path '*/System Volume Information' -prune -o \
        -type d -print -o \
        -type f ! -name '*-wal' ! -name '*-shm' ! -name '*-journal' \
        -exec stat -f '%N %m %z' {} + 2>/dev/null | sort
}
tolerated() {  # what the reads are allowed to leave behind, reported not hidden
    find "$1" -mindepth 1 \
        \( -path "$1/Seabass/caches" -o -name '*-wal' -o -name '*-shm' -o -name '*-journal' \) \
        2>/dev/null | sed "s|^$1/||" | sort
}
before="$work/before.txt"
stick_tree "$stick" > "$before"
tolerated "$stick" > "$work/tolerated-before.txt"

fail=${fail_early:-0}
for arch in $arches; do
    echo "== $arch slice"
    # </dev/null: scan asks Console::confirm() before consolidating a
    # duplicate-cue group, and isInteractive() is stdin-isatty. Run from a
    # terminal without this it waits for ever at a prompt whose text went
    # into the log file -- and a "y" would write to the stick, which this
    # check promises not to do. tools/rig-shakedown.sh does the same.
    run() { arch -"$arch" "$cli" "$@" </dev/null; }
    if ! run --help >/dev/null 2>&1; then
        echo "  does not run at all" >&2; fail=1; continue
    fi
    # The libraries dyld actually loaded, where it will say. arch passes
    # DYLD_PRINT_LIBRARIES on with -e: set in our own environment it would
    # be purged at /usr/bin/arch, which SIP protects. dyld may still ignore
    # it for the binary (a restricted process; possibly a hardened-runtime
    # signature, which the Developer ID packages carry), and then there is
    # nothing to read and nothing to be done about it on CI: making the
    # Craft root unreadable for one run is not possible there. The static
    # check above is the gate; this is a second opinion when it is given.
    loaded="$work/loaded-$arch.log"
    arch -"$arch" -e DYLD_PRINT_LIBRARIES=1 "$cli" --help </dev/null >/dev/null 2>"$loaded"
    # "dyld[pid]: <uuid> /path" from dyld4, "dyld: loaded: /path" before it.
    sed -n -e 's/^dyld\[[0-9]*\]: \(<[^>]*> \)\{0,1\}\(\/.*\)$/\2/p' \
        -e 's/^dyld: loaded: \(\/.*\)$/\1/p' "$loaded" > "$loaded.paths"
    if [ ! -s "$loaded.paths" ]; then
        echo "  dyld printed no loaded libraries (DYLD_PRINT_LIBRARIES ignored for this binary): relying on the static check"
    else
        real_app="$(cd "$app" && pwd -P)"
        outside="$(awk -v a="$app/" -v r="$real_app/" 'index($0, a) != 1 && index($0, r) != 1 &&
            index($0, "/System/Library/") != 1 && index($0, "/usr/lib/") != 1' "$loaded.paths")"
        if [ -n "$outside" ]; then
            echo "  LOADED FROM OUTSIDE THE BUNDLE:" >&2
            printf '%s\n' "$outside" | sed 's/^/    /' >&2
            fail=1
        else
            echo "  loaded $(wc -l < "$loaded.paths" | tr -d ' ') images, all from the bundle or the system"
        fi
    fi
    out="$work/scan-$arch.log"
    run scan --rekordbox "$stick/PIONEER" >"$out" 2>&1
    rc=$?
    # The counts, not the decoration: they are what gets compared between
    # the slices, and a difference means one of them read the stick wrongly.
    tracks="$(count_of "$out" tracks)"
    cues="$(count_of "$out" "total cues")"
    echo "  scan: tracks ${tracks:-none}, total cues ${cues:-none}"
    [ "$rc" -eq 0 ] || { echo "  scan failed ($rc)" >&2; fail=1; }
    if [ -z "$tracks" ] || [ -z "$cues" ]; then
        echo "  scan printed no track or cue count, so there is nothing to compare" >&2
        tail -3 "$out" | sed 's/^/    /' >&2
        fail=1
    fi
    pinned "scan tracks" "$tracks" "$expect_rb_tracks" || fail=1
    pinned "scan cues" "$cues" "$expect_rb_cues" || fail=1
    printf 'tracks %s\ncues %s\n' "$tracks" "$cues" > "$work/counts-$arch.txt"

    # sync --dry-run is what opens OneLibrary (SQLCipher) beside export.pdb,
    # so its OneLibrary line is required, not merely one catalog line of
    # three, and its exit status counts: a sync that printed the rekordbox
    # count and then failed on OneLibrary has not shown SQLCipher works.
    sout="$work/sync-$arch.log"
    run sync --rekordbox "$stick/PIONEER" --engine "$stick/Engine Library" --dry-run >"$sout" 2>&1
    src_rc=$?
    rb="$(count_of "$sout" "rekordbox tracks")"
    one="$(count_of "$sout" "onelibrary tracks")"
    matched="$(count_of "$sout" "matched tracks")"
    echo "  sync: rekordbox ${rb:-none}, onelibrary ${one:-none}, matched ${matched:-none}"
    if [ "$src_rc" -ne 0 ]; then
        echo "  sync --dry-run failed ($src_rc):" >&2
        tail -3 "$sout" | sed 's/^/    /' >&2
        fail=1
    fi
    if [ -z "$one" ]; then
        echo "  sync --dry-run printed no onelibrary count, so SQLCipher is unchecked" >&2
        fail=1
    fi
    pinned "sync rekordbox tracks" "$rb" "$expect_rb_tracks" || fail=1
    pinned "sync onelibrary tracks" "$one" "$expect_one_tracks" || fail=1
    printf 'rekordbox %s\nonelibrary %s\nmatched %s\n' "$rb" "$one" "$matched" > "$work/one-$arch.txt"
done

echo "== the two slices agree"
set -- $arches
if [ $# -ge 2 ]; then
    for f in counts one; do
        if [ ! -f "$work/$f-$1.txt" ] || [ ! -f "$work/$f-$2.txt" ]; then
            echo "  $f: missing for one of the slices, so there is nothing to compare" >&2
            fail=1
            continue
        fi
        if cmp -s "$work/$f-$1.txt" "$work/$f-$2.txt"; then
            echo "  $f: identical"
        else
            echo "  $f: DIFFER between $1 and $2" >&2
            diff "$work/$f-$1.txt" "$work/$f-$2.txt" | sed 's/^/    /' >&2
            fail=1
        fi
    done
else
    echo "  only one architecture in this package, nothing to compare"
fi

echo "== the stick is unchanged"
after="$work/after.txt"
stick_tree "$stick" > "$after"
tolerated "$stick" > "$work/tolerated-after.txt"
if cmp -s "$before" "$after"; then
    echo "  every file of the library is byte-for-byte as it was"
else
    echo "  THE STICK CHANGED -- a read-only check wrote to it:" >&2
    diff "$before" "$after" | head -20 | sed 's/^/    /' >&2
    fail=1
fi
# Said out loud rather than passed over: these are expected, but a reader
# should see what reading left behind.
if ! cmp -s "$work/tolerated-before.txt" "$work/tolerated-after.txt"; then
    echo "  derived files the reads left behind (allowed, as in the rig):"
    diff "$work/tolerated-before.txt" "$work/tolerated-after.txt" | grep '^>' | sed 's/^> /    /'
fi

echo
[ "$fail" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
