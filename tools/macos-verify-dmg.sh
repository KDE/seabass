#!/bin/bash
# Check a macOS .dmg against a real stick, once per architecture it carries.
#
#   tools/macos-verify-dmg.sh <dmg> <stick mount point>
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
set -u
set -o pipefail
dmg="${1:?the .dmg to check}"
stick="${2:?a stick mount point, e.g. /Volumes/VSTICKB}"
# Trailing slashes come free from tab-completion and broke the -prune paths
# below, which then reported a perfectly good package as having written to
# the stick.
while [ "${stick%/}" != "$stick" ]; do stick="${stick%/}"; done
[ -f "$dmg" ] || { echo "no such file: $dmg" >&2; exit 1; }
[ -d "$stick" ] || { echo "not mounted: $stick" >&2; exit 1; }

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
echo "== architectures in the bundle"
total=0; fat=0; thin=""
while IFS= read -r f; do
    archs="$(lipo -archs "$f" 2>/dev/null)" || continue
    [ -z "$archs" ] && continue
    total=$((total+1))
    case "$archs" in
        *" "*) fat=$((fat+1)) ;;
        *) thin="$thin\n  thin: ${f#$app/} -> $archs" ;;
    esac
done < <(find "$app" -type f)
echo "  $total Mach-O files, $fat with more than one architecture"
if [ -n "$thin" ]; then
    printf "%b\n" "$thin" >&2
    echo "  a package whose binaries are not all universal is the defect this checks for" >&2
    fail_early=1
fi
[ "$total" -gt 0 ] || { echo "  no Mach-O files found in the bundle at all" >&2; exit 1; }
if [ ! -x "$cli" ]; then
    echo "  no seabass-cli in the bundle at Contents/MacOS/seabass-cli" >&2; exit 1
fi
arches="$(lipo -archs "$cli" 2>/dev/null || true)"
if [ -z "$arches" ]; then
    echo "  seabass-cli is not a Mach-O binary, so there is nothing to check" >&2; exit 1
fi
echo "  seabass-cli: $arches"
# The package must carry both, and saying so is the entire point of this
# branch: without it an arm64-only .dmg -- the defect being fixed -- passed.
case "$arches" in
    *x86_64*arm64*|*arm64*x86_64*) : ;;
    *) echo "  NOT UNIVERSAL: seabass-cli carries only $arches" >&2; fail_early=1 ;;
esac

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
    out="$work/scan-$arch.log"
    run scan --rekordbox "$stick/PIONEER" >"$out" 2>&1
    rc=$?
    # The counts, not the decoration: they are what gets compared between
    # the slices, and a difference means one of them read the stick wrongly.
    counts="$(tr -d '\000' < "$out" | sed 's/\x1b\[[0-9;]*m//g' | grep -E 'tracks:|total cues:' | tr -s ' ')"
    echo "$counts" | sed 's/^/  /'
    [ "$rc" -eq 0 ] || { echo "  scan failed ($rc)" >&2; fail=1; }
    if [ -z "$counts" ]; then
        echo "  scan printed no track or cue counts, so there is nothing to compare" >&2
        tail -3 "$out" | sed 's/^/    /' >&2
        fail=1
    fi
    echo "$counts" > "$work/counts-$arch.txt"

    # sync --dry-run is what opens OneLibrary (SQLCipher) beside export.pdb.
    sout="$work/sync-$arch.log"
    run sync --rekordbox "$stick/PIONEER" --engine "$stick/Engine Library" --dry-run >"$sout" 2>&1
    src_rc=$?
    one="$(tr -d '\000' < "$sout" | sed 's/\x1b\[[0-9;]*m//g' | grep -E 'onelibrary tracks:|rekordbox tracks:|matched tracks:' | tr -s ' ')"
    if [ -n "$one" ]; then
        echo "$one" | sed 's/^/  /'
        echo "$one" > "$work/one-$arch.txt"
    else
        echo "  no catalog line from sync --dry-run (exit $src_rc):" >&2
        tail -3 "$sout" | sed 's/^/    /' >&2
        fail=1
    fi
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
