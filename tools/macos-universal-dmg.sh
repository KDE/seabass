#!/bin/bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Merge a Craft-built arm64 Seabass.app and an x86_64 one into a single
# universal bundle, and wrap it in a .dmg.
#
#   tools/macos-universal-dmg.sh <arm64.app> <x86_64.app> <out.dmg> [volume name]
#   tools/macos-universal-dmg.sh --from-dmgs <arm64.dmg> <x86_64.dmg> \
#       <arm64 install.db> <x86_64 install.db> <out.dmg> [volume name]
#
# The second form takes what the two Craft CI jobs leave: their .dmg files,
# and the install.db each copied out of its Craft root before cleaning up.
#
# SEABASS_SIGN_COMMAND, if set, is run with the merged .app and then with
# the .dmg appended (split on whitespace, like Craft's MacCustomSignCommand),
# and each must come back with a real signature, not an ad-hoc one.
# SEABASS_NOTARIZE_COMMAND, if set, is run with the .dmg appended, and the
# .dmg must then carry a stapled ticket.
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
# Ad-hoc signed unless SEABASS_SIGN_COMMAND says otherwise.
set -u
set -o pipefail

work="$(mktemp -d "${TMPDIR:-/tmp}/seabass-universal.XXXXXX")"
attached=()
cleanup() {
    # Detached before the temp tree goes: rm -rf over a live mount would
    # leave the image attached.
    local mp
    for mp in ${attached[@]+"${attached[@]}"}; do
        hdiutil detach "$mp" -quiet 2>/dev/null || hdiutil detach "$mp" -force -quiet 2>/dev/null ||
            echo "could not detach $mp: the image is still attached" >&2
    done
    rm -rf "$work"
}
trap cleanup EXIT

# <dmg> <arch>: copies the one .app in the image to $work/<arch>/ and
# leaves its path in $extracted. Not called in $(...): a subshell would
# keep a failed attach out of $attached, and cleanup would miss it.
app_from_dmg() {
    local mp="$work/mnt-$2" apps
    mkdir -p "$mp" "$work/$2"
    hdiutil attach -readonly -nobrowse -noautoopen -mountpoint "$mp" "$1" >/dev/null ||
        { echo "could not attach $1" >&2; return 1; }
    attached+=("$mp")
    apps=("$mp"/*.app)
    if [ "${#apps[@]}" -ne 1 ] || [ ! -d "${apps[0]}" ]; then
        echo "$1 does not hold exactly one .app" >&2; return 1
    fi
    cp -R "${apps[0]}" "$work/$2/" || return 1
    hdiutil detach "$mp" -quiet || { echo "could not detach $mp" >&2; return 1; }
    unset 'attached[${#attached[@]}-1]'
    extracted="$work/$2/$(basename "${apps[0]}")"
}

arm_db=""; intel_db=""
if [ "${1:-}" = "--from-dmgs" ]; then
    arm_dmg="${2:?the arm64 .dmg}"
    intel_dmg="${3:?the x86_64 .dmg}"
    arm_db="${4:?the arm64 install.db}"
    intel_db="${5:?the x86_64 install.db}"
    out_dmg="${6:?the .dmg to write}"
    volume="${7:-Seabass}"
    for f in "$arm_dmg" "$intel_dmg" "$arm_db" "$intel_db"; do
        [ -f "$f" ] || { echo "no such file: $f" >&2; exit 1; }
    done
    echo "== copying the bundles out of the two images"
    app_from_dmg "$arm_dmg" arm64 || exit 1; arm="$extracted"
    app_from_dmg "$intel_dmg" x86_64 || exit 1; intel="$extracted"
else
    arm="${1:?the arm64 .app}"
    intel="${2:?the x86_64 .app}"
    out_dmg="${3:?the .dmg to write}"
    volume="${4:-Seabass}"
fi

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
if [ -z "$arm_db" ]; then
    arm_root="$(craft_root_of "$arm")" && arm_db="$arm_root/etc/blueprints/install.db"
    intel_root="$(craft_root_of "$intel")" && intel_db="$intel_root/etc/blueprints/install.db"
fi
echo "== the two Craft roots agree on package versions"
if [ -z "$arm_db" ] || [ -z "$intel_db" ] || ! command -v sqlite3 >/dev/null 2>&1; then
    # Said out loud and refused rather than skipped: a check that did not run
    # has proved nothing, and this one is the only thing standing between a
    # universal package and two different builds of ffmpeg, one per slice.
    why="the bundles are not in a Craft root's layout"
    command -v sqlite3 >/dev/null 2>&1 || why="sqlite3 is not on PATH"
    echo "  CANNOT CHECK: $why" >&2
    if [ -n "${SEABASS_UNCHECKED_ROOTS:-}" ] && [ "${1:-}" != "--from-dmgs" ]; then
        echo "  continuing because SEABASS_UNCHECKED_ROOTS is set" >&2
    else
        echo "  set SEABASS_UNCHECKED_ROOTS=1 to merge two .app bundles anyway, knowing this was not checked." >&2
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
    # Each list is read on its own and must be non-empty: two databases that
    # sqlite3 cannot read give two empty lists, and those are identical.
    for side in arm intel; do
        db="${side}_db"
        if ! sqlite3 "${!db}" "$q" > "$work/packages-$side.txt" 2> "$work/packages-$side.err" ||
           [ ! -s "$work/packages-$side.txt" ]; then
            echo "  CANNOT CHECK: no package list from ${!db}" >&2
            sed 's/^/    /' "$work/packages-$side.err" >&2
            exit 1
        fi
    done
    if ! diff "$work/packages-arm.txt" "$work/packages-intel.txt" > "$work/packages.diff"; then
        echo "  the roots differ, and a merged bundle would carry one build of each:" >&2
        sed 's/^/    /' "$work/packages.diff" >&2
        echo "  bring them level first, e.g. craft --update <package> in the older root, then re-package." >&2
        exit 1
    fi
    echo "  identical ($(wc -l < "$work/packages-arm.txt" | tr -d ' ') packages)"
fi

app="$work/merged/$(basename "$arm")"
mkdir -p "$work/merged"
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
    # Rosetta is asked about first, so that a machine without it is named
    # as the reason rather than read as a broken x86_64 slice.
    if ! arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
        echo "  ONLY THE ARM64 SLICE RAN: this machine cannot execute x86_64 code (no Rosetta)," >&2
        echo "  so the x86_64 slice is unchecked and this package is refused." >&2
        exit 1
    fi
    if arch -x86_64 "$cli" --help >/dev/null 2>&1 </dev/null; then echo "  x86_64 slice runs (under Rosetta)"; else
        echo "  the x86_64 slice does not run, although Rosetta does" >&2; exit 1; fi
else
    echo "  no seabass-cli in the bundle to run" >&2; exit 1
fi

# A signing command that finds its ref not cleared for signing skips and
# exits 0 (ci-notary-service's signmacapp.py does), so its status proves
# nothing: the signature is read back instead.
sign_cmd=(); notarize_cmd=()
[ -n "${SEABASS_SIGN_COMMAND:-}" ] && read -r -a sign_cmd <<< "$SEABASS_SIGN_COMMAND"
[ -n "${SEABASS_NOTARIZE_COMMAND:-}" ] && read -r -a notarize_cmd <<< "$SEABASS_NOTARIZE_COMMAND"
really_signed() {  # <path>: a signature with an Authority, not an ad-hoc one
    local out
    out="$(codesign -dvv "$1" 2>&1)" || { printf '%s\n' "$out" | sed 's/^/  /' >&2; return 1; }
    if printf '%s\n' "$out" | grep -q '^Signature=adhoc' || ! printf '%s\n' "$out" | grep -q '^Authority='; then
        printf '%s\n' "$out" | sed 's/^/  /' >&2; return 1
    fi
    printf '%s\n' "$out" | grep -E '^(Authority|TeamIdentifier|Timestamp)=' | sed 's/^/  /'
}
if [ "${#sign_cmd[@]}" -gt 0 ]; then
    echo "== signing the merged bundle"
    "${sign_cmd[@]}" "$app" || { echo "  the signing command failed" >&2; exit 1; }
    really_signed "$app" || { echo "  the bundle is not signed for distribution after signing" >&2; exit 1; }
    codesign --verify --deep --strict "$app" || { echo "  the signed bundle does not verify" >&2; exit 1; }
    # The signature is new, so the slices are run again under it.
    arch -arm64 "$cli" --help >/dev/null 2>&1 </dev/null || { echo "  the signed arm64 slice does not run" >&2; exit 1; }
    arch -x86_64 "$cli" --help >/dev/null 2>&1 </dev/null || { echo "  the signed x86_64 slice does not run" >&2; exit 1; }
    echo "  signed, verifies, both slices still run"
fi

echo "== building $out_dmg"
rm -f "$out_dmg"
staging="$work/dmg"
mkdir -p "$staging"
cp -R "$app" "$staging/"
ln -s /Applications "$staging/Applications"
# HFS+ as Craft's dmgbuild makes it, rather than whatever hdiutil defaults to.
hdiutil create -volname "$volume" -srcfolder "$staging" -fs HFS+ -ov -format UDZO -quiet "$out_dmg" || exit 1
if [ "${#sign_cmd[@]}" -gt 0 ]; then
    echo "== signing the image"
    "${sign_cmd[@]}" "$out_dmg" || { echo "  the signing command failed" >&2; exit 1; }
    really_signed "$out_dmg" || { echo "  the image is not signed for distribution after signing" >&2; exit 1; }
fi
if [ "${#notarize_cmd[@]}" -gt 0 ]; then
    echo "== notarising the image"
    "${notarize_cmd[@]}" "$out_dmg" || { echo "  the notarising command failed" >&2; exit 1; }
    xcrun stapler validate "$out_dmg" || { echo "  no notarisation ticket is stapled to the image" >&2; exit 1; }
fi
# Written with the bare filename, so `shasum -c` works wherever the package
# is downloaded to; with the path as given it only checked on this machine.
( cd "$(dirname "$out_dmg")" && shasum -a 256 "$(basename "$out_dmg")" | tee "$(basename "$out_dmg").sha256" )
echo "done"
