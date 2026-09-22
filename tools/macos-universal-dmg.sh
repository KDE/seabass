#!/bin/bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Merge a Craft-built arm64 Seabass.app and an x86_64 one into a single
# universal bundle, and wrap it in a .dmg.
#
#   tools/macos-universal-dmg.sh <arm64.app> <x86_64.app> <out.dmg> [volume name]
#
# Why this exists: Rosetta translates x86_64 to ARM and never the reverse,
# so an arm64-only package mounts on an Intel Mac and refuses to launch.
# Craft builds one architecture per root and has no universal mode, so the
# two packages are merged afterwards.
#
# The merge is not a copy. Three things differ between the two bundles and
# each needs its own answer:
#   - Mach-O files: lipo'd together. That is the point of the exercise.
#   - _CodeSignature/CodeResources: hashes OF those binaries, so they are
#     wrong for the merged result whichever side they come from. Every
#     framework is re-signed afterwards, innermost first.
#   - Files recording where they were built: textually different, same
#     meaning. Reported, not treated as a fault.
# Ad-hoc signed only. Signing and notarisation belong to CI.
set -u
set -o pipefail
arm="${1:?the arm64 .app}"
intel="${2:?the x86_64 .app}"
out_dmg="${3:?the .dmg to write}"
volume="${4:-Seabass}"

# A DEPLOYED bundle, not an installed one. Craft's <root>/Applications/KDE/
# seabass.app holds four files -- the executable, Info.plist, the icon and a
# signature -- because Qt is still in the root's lib/. The bundle with Qt
# inside it only exists after `craft --package`, under
# <root>/build/qt-apps/seabass/archive/Applications/KDE/seabass.app, and
# that is the one to merge. Handed the installed one, this script merged a
# single binary, reported success, and produced a .dmg that would have
# launched on neither architecture.
MinimumMachO=50
for app in "$arm" "$intel"; do
    [ -d "$app" ] || { echo "not a bundle: $app" >&2; exit 1; }
    n=$(find "$app" -type f -exec file -b {} + 2>/dev/null | grep -c "Mach-O")
    if [ "$n" -lt "$MinimumMachO" ]; then
        echo "$app holds only $n Mach-O files: that is an installed bundle, not a packaged one." >&2
        echo "Run 'craft --package seabass' and use <root>/build/qt-apps/seabass/archive/Applications/KDE/seabass.app" >&2
        exit 1
    fi
done

# The two Craft roots must hold the same package versions, and the bundles
# cannot always show it. libvpx 1.15.2 against 1.16.0 changed the soname, so
# the file-name check below caught it -- but ffmpeg 8.1.1-4 against 8.1.1-6
# keeps every filename, and would have merged two different ffmpeg builds
# into one bundle, one per slice, with nothing anywhere to say so. So ask
# the roots directly when the bundles sit in a recognisable Craft layout.
craft_root_of() {  # <root>/build/qt-apps/seabass/archive/Applications/KDE/x.app
    local d="${1%/Applications/KDE/*}"
    d="${d%/build/qt-apps/seabass/archive}"
    [ -f "$d/etc/blueprints/install.db" ] && echo "$d"
}
arm_root="$(craft_root_of "$arm")" || true
intel_root="$(craft_root_of "$intel")" || true
echo "== the two Craft roots agree on package versions"
if [ -z "${arm_root:-}" ] || [ -z "${intel_root:-}" ] || ! command -v sqlite3 >/dev/null 2>&1; then
    # Said out loud and refused rather than skipped: a check that did not run
    # has proved nothing, and this one is the only thing standing between a
    # universal package and two different builds of ffmpeg, one per slice.
    why="the bundles are not in a Craft root's layout"
    command -v sqlite3 >/dev/null 2>&1 || why="sqlite3 is not on PATH"
    echo "  CANNOT CHECK: $why" >&2
    if [ -n "${SEABASS_UNCHECKED_ROOTS:-}" ]; then
        echo "  continuing because SEABASS_UNCHECKED_ROOTS is set" >&2
    else
        echo "  set SEABASS_UNCHECKED_ROOTS=1 to merge anyway, knowing this was not checked." >&2
        exit 1
    fi
else
    # revision, not just version: the qt-apps/seabass row carries version
    # "master" with the BRANCH in revision, so comparing version alone would
    # call two roots built from different Seabass branches identical -- and
    # then lipo succeeds, differing QML is only reported, and the package
    # runs different application code depending on the CPU.
    # craft/* is Craft's own machinery -- craft-core and the blueprint clone
    # itself. Those carry date versions that drift between roots and ship
    # nothing into the bundle, so comparing them refuses good merges for a
    # difference no user could ever see. Everything that does contribute is
    # compared, and by revision as well as version.
    q="select packagePath || ' ' || version || ' ' || ifnull(revision, '')
       from packageList where packagePath not like 'craft/%' order by packagePath;"
    if ! diff <(sqlite3 "$arm_root/etc/blueprints/install.db" "$q") \
              <(sqlite3 "$intel_root/etc/blueprints/install.db" "$q") > "${TMPDIR:-/tmp}/seabass-pkgdiff.$$"; then
        echo "  the roots differ, and a merged bundle would carry one build of each:" >&2
        sed 's/^/    /' "${TMPDIR:-/tmp}/seabass-pkgdiff.$$" >&2
        rm -f "${TMPDIR:-/tmp}/seabass-pkgdiff.$$"
        echo "  bring them level first, e.g. craft --update <package> in the older root, then re-package." >&2
        exit 1
    fi
    rm -f "${TMPDIR:-/tmp}/seabass-pkgdiff.$$"
    echo "  identical"
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/seabass-universal.XXXXXX")"
trap 'rm -rf "$work"' EXIT
app="$work/$(basename "$arm")"
echo "== staging from the arm64 bundle"
cp -R "$arm" "$app" || exit 1

is_macho() { file -b "$1" 2>/dev/null | grep -q "Mach-O"; }

merged=0; copied=0; armonly=0; intelonly=0; differing=0
echo "== merging Mach-O files"
while IFS= read -r rel; do
    a="$app/$rel"; i="$intel/$rel"
    [ -L "$a" ] && continue
    if [ ! -e "$i" ]; then
        if is_macho "$a"; then
            echo "  ONLY IN ARM64: $rel"; armonly=$((armonly+1))
        fi
        continue
    fi
    if is_macho "$a"; then
        if lipo -create -output "$a.universal" "$a" "$i" 2>/dev/null && mv "$a.universal" "$a"; then
            merged=$((merged+1))
        else
            echo "  LIPO FAILED: $rel" >&2; exit 1
        fi
    else
        case "$rel" in
            *_CodeSignature*) : ;;  # regenerated below, never compared
            *)
                if ! cmp -s "$a" "$i"; then
                    echo "  differs (kept arm64's): $rel"; differing=$((differing+1))
                fi
                copied=$((copied+1)) ;;
        esac
    fi
done < <(cd "$app" && find . -type f | sed 's|^\./||' | sort)

# Anything the Intel bundle has that the arm64 one does not would be
# silently missing from the merge, so it is counted rather than assumed.
while IFS= read -r rel; do
    [ -e "$app/$rel" ] || { echo "  ONLY IN X86_64: $rel"; intelonly=$((intelonly+1)); }
done < <(cd "$intel" && find . -type f | sed 's|^\./||' | sort)

echo "  merged $merged, non-binary $copied ($differing differing), arm64-only $armonly, x86_64-only $intelonly"
[ "$armonly" -eq 0 ] && [ "$intelonly" -eq 0 ] || { echo "the two bundles do not hold the same files" >&2; exit 1; }

echo "== re-signing (innermost first: a framework's CodeResources hashes the binary we just replaced)"
while IFS= read -r fw; do
    codesign --force --sign - --timestamp=none "$fw" >/dev/null 2>&1 || {
        echo "  could not sign $fw" >&2; exit 1; }
done < <(find "$app" -name '*.framework' -type d -depth 1 -o -name '*.framework' -type d | sort -r)
while IFS= read -r dylib; do
    codesign --force --sign - --timestamp=none "$dylib" >/dev/null 2>&1 || {
        echo "  could not sign ${dylib#$app/}" >&2; exit 1; }
done < <(find "$app" -name '*.dylib' -type f)
codesign --force --deep --sign - --timestamp=none "$app" >/dev/null 2>&1 || {
    echo "  could not sign the bundle" >&2; exit 1; }
# Status taken from codesign itself, not from a pipeline ending in sed:
# without this the one gate on the re-signing step could not fail.
if verify_out="$(codesign --verify --deep --strict "$app" 2>&1)"; then
    [ -z "$verify_out" ] || printf '%s\n' "$verify_out" | sed 's/^/  /'
    echo "  signature verifies"
else
    printf '%s\n' "$verify_out" | sed 's/^/  /' >&2
    echo "the merged bundle does not verify" >&2
    exit 1
fi

echo "== every Mach-O carries both architectures"
bad=0; total=0
while IFS= read -r f; do
    archs="$(lipo -archs "$f" 2>/dev/null)" || continue
    [ -z "$archs" ] && continue
    total=$((total+1))
    case "$archs" in
        *x86_64*arm64*|*arm64*x86_64*) : ;;
        *) echo "  NOT UNIVERSAL: ${f#$app/} -> $archs"; bad=$((bad+1)) ;;
    esac
done < <(find "$app" -type f)
echo "  $total Mach-O files, $bad not universal"
[ "$bad" -eq 0 ] || exit 1

# Both slices are made to run, because a universal binary that carries an
# architecture it cannot execute looks identical to lipo.
echo "== both slices run"
cli="$app/Contents/MacOS/seabass-cli"
if [ -x "$cli" ]; then
    if arch -arm64 "$cli" --help >/dev/null 2>&1 </dev/null; then echo "  arm64 slice runs"; else
        echo "  the arm64 slice does not run -- is this an Apple Silicon Mac?" >&2
        echo "  (an Intel host cannot execute an arm64 slice at all, so the merge step belongs on Apple Silicon)" >&2
        exit 1; fi
    if arch -x86_64 "$cli" --help >/dev/null 2>&1 </dev/null; then echo "  x86_64 slice runs (under Rosetta)"; else
        echo "  the x86_64 slice does not run -- is Rosetta installed?" >&2; exit 1; fi
else
    echo "  no seabass-cli in the bundle to run" >&2; exit 1
fi

echo "== building $out_dmg"
rm -f "$out_dmg"
staging="$work/dmg"
mkdir -p "$staging"
cp -R "$app" "$staging/"
ln -s /Applications "$staging/Applications"
hdiutil create -volname "$volume" -srcfolder "$staging" -ov -format UDZO -quiet "$out_dmg" || exit 1
# Written with the bare filename, so `shasum -c` works wherever the package
# is downloaded to; with the path as given it only checked on this machine.
( cd "$(dirname "$out_dmg")" && shasum -a 256 "$(basename "$out_dmg")" | tee "$(basename "$out_dmg").sha256" )
echo "done"
