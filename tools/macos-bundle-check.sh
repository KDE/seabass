# shellcheck shell=bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Sourced by tools/macos-universal-dmg.sh and tools/macos-verify-dmg.sh,
# so that the two cannot come to disagree about what a shippable bundle is.
#
#   seabass_bundle_self_contained <app>
#
# Returns 0 when the bundle can start on a Mac that has nothing but macOS,
# and 1 otherwise, having said on stderr what is wrong, naming the file and
# the load command.
#
# Why: a Mac trial merged two halves of 9.6 and 8.7 MB, where a deployed
# Seabass bundle is well over 100 MB, and every later step passed. The
# halves still found Qt through an absolute path or an LC_RPATH into that
# Mac's Craft root, so they ran there and would have run nowhere else. So
# this reads what the binaries ASK for, rather than whether they start on
# the machine that built them:
#   - every LC_LOAD_DYLIB (and its weak, re-export, lazy and upward kinds)
#     names either a system library (/System/Library/..., /usr/lib/...) or a
#     file inside the bundle, through @executable_path, @loader_path or
#     @rpath; any other absolute path is refused (a Craft root, a home
#     directory, /opt, /usr/local, and the bundle's own location too, which
#     is only right on the machine that built it);
#   - every LC_RPATH resolves inside the bundle, or to the system;
#   - an @rpath load must find its file through some LC_RPATH in the bundle
#     (a superset of what dyld searches, so a miss here is a miss there);
#   - Contents/Frameworks holds QtCore.framework and every Qt framework the
#     executables in Contents/MacOS link;
#   - the bundle is at least SEABASS_BUNDLE_MIN_MB (100) MB unpacked, since
#     Craft's installed, undeployed bundle is a few MB and its Qt lives in
#     the root.
# otool and file are taken from PATH, which is how the test on Linux feeds
# in canned load commands (tests/macos_bundle_check_test.sh).

# Lexically normalised: "." and ".." folded, no symlinks followed, so a
# path cannot leave the bundle by "..", and the answer does not depend on
# what exists on this machine.
_sbc_normalize() {
    local part out=() parts=()
    IFS=/ read -r -a parts <<< "$1"
    for part in ${parts[@]+"${parts[@]}"}; do
        case "$part" in
            ''|.) ;;
            ..) [ "${#out[@]}" -gt 0 ] && out=("${out[@]:0:${#out[@]}-1}") ;;
            *) out+=("$part") ;;
        esac
    done
    if [ "${#out[@]}" -eq 0 ]; then echo /; else printf '/%s' "${out[@]}"; echo; fi
}

_sbc_is_system() {
    case "$1" in /System/Library/*|/usr/lib/*) return 0 ;; esac
    return 1
}

# <path> <app>: inside the bundle (the bundle directory itself included).
_sbc_is_inside() {
    case "$1" in "$2"|"$2"/*) return 0 ;; esac
    return 1
}

# <path as written> <the Mach-O it is written in> <app>: the absolute,
# normalised path, or nothing when it cannot be expanded (@rpath, a
# relative path).
_sbc_expand() {
    local exe_dir
    case "$2" in
        */Contents/MacOS/*) exe_dir="${2%/*}" ;;
        *) exe_dir="$3/Contents/MacOS" ;;
    esac
    case "$1" in
        @executable_path/*) _sbc_normalize "$exe_dir/${1#@executable_path/}" ;;
        @loader_path/*) _sbc_normalize "${2%/*}/${1#@loader_path/}" ;;
        @executable_path) _sbc_normalize "$exe_dir" ;;
        @loader_path) _sbc_normalize "${2%/*}" ;;
        /*) _sbc_normalize "$1" ;;
        *) : ;;
    esac
}

# `otool -l` reduced to "<command><TAB><path>" for the commands that name a
# library or a search path. A fat file lists each slice's commands in turn,
# and all of them are checked.
_sbc_load_commands() {
    local out
    out="$(otool -l "$1")" || return 1
    printf '%s\n' "$out" | awk '
        $1 == "cmd" { cmd = $2; next }
        (cmd ~ /^LC_(LOAD_DYLIB|LOAD_WEAK_DYLIB|REEXPORT_DYLIB|LAZY_LOAD_DYLIB|LOAD_UPWARD_DYLIB)$/ && $1 == "name") ||
        (cmd == "LC_RPATH" && $1 == "path") {
            line = $0
            sub(/^[ \t]*(name|path) /, "", line)
            sub(/ \(offset [0-9]+\)[ \t]*$/, "", line)
            print cmd "\t" line
            cmd = ""
        }'
}

# <path as written> <app> <where>: an absolute path that is not the
# system's is refused even when it points into the bundle, because it only
# points there while the bundle sits where it was built. Adds the problem
# and returns 0 when it is one; uses the caller's problems array.
_sbc_absolute_nonsystem() {
    case "$1" in
        /*) ;;
        *) return 1 ;;
    esac
    if _sbc_is_inside "$(_sbc_normalize "$1")" "$2"; then
        problems+=("$3 $1 is an absolute path: it finds the bundle only where it was built")
    else
        problems+=("$3 $1 lies outside the bundle")
    fi
    return 0
}

# <path as written> <app> <rel>: a Qt framework an executable links must be
# in Contents/Frameworks. Uses the caller's problems array.
_sbc_qt_framework_present() {
    local fw
    case "$1" in
        */Qt*.framework/*)
            fw="${1%%.framework/*}"; fw="${fw##*/}.framework"
            [ -d "$2/Contents/Frameworks/$fw" ] ||
                problems+=("$3: links $fw, which Contents/Frameworks does not hold")
            ;;
    esac
}

seabass_bundle_self_contained() {
    local given="$1" app
    app="$(cd "$given" 2>/dev/null && pwd)" || { echo "  not a bundle: $given" >&2; return 1; }
    app="$(_sbc_normalize "$app")"
    local problems=() min_mb="${SEABASS_BUNDLE_MIN_MB:-100}"

    local kb
    kb="$(du -sk "$app" | awk '{ print $1 }')"
    if [ -z "$kb" ] || [ "$kb" -lt $((min_mb * 1024)) ]; then
        problems+=("the bundle is only $((${kb:-0} / 1024)) MB unpacked, under the $min_mb MB floor: that looks like Craft's undeployed stub (<root>/Applications/KDE/seabass.app), whose Qt and FFmpeg are still in the Craft root. Use the packaged bundle, <root>/build/qt-apps/seabass/archive/Applications/KDE/seabass.app")
    fi
    [ -d "$app/Contents/Frameworks/QtCore.framework" ] ||
        problems+=("Contents/Frameworks has no QtCore.framework: Qt is not deployed into this bundle")

    # Every Mach-O, found by file(1) in one batch. "//" as the separator:
    # find never prints it inside a path.
    local machos=() line
    while IFS= read -r line; do
        case "${line#*//}" in *Mach-O*) machos+=("${line%%//*}") ;; esac
    done < <(find "$app" -type f -exec file -N -F // {} + 2>/dev/null)
    if [ "${#machos[@]}" -eq 0 ]; then
        problems+=("no Mach-O file in the bundle at all")
    fi

    # First pass: rpaths and every load that can be judged on its own.
    # @rpath loads wait for the second, once every LC_RPATH is known.
    local rpaths=() deferred=() f rel cmd value target commands
    for f in ${machos[@]+"${machos[@]}"}; do
        rel="${f#"$app"/}"
        if ! commands="$(_sbc_load_commands "$f")"; then
            problems+=("$rel: otool -l could not read it"); continue
        fi
        while IFS=$'\t' read -r cmd value; do
            [ -n "$cmd" ] || continue
            if [ "$cmd" = LC_RPATH ]; then
                target="$(_sbc_expand "$value" "$f" "$app")"
                if [ -z "$target" ]; then
                    problems+=("$rel: LC_RPATH $value cannot be resolved to a directory")
                elif _sbc_is_system "$target"; then
                    :
                elif _sbc_absolute_nonsystem "$value" "$app" "$rel: LC_RPATH"; then
                    :
                elif _sbc_is_inside "$target" "$app"; then
                    rpaths+=("$target")
                else
                    problems+=("$rel: LC_RPATH $value lies outside the bundle")
                fi
                continue
            fi
            case "$rel" in
                Contents/MacOS/*/*) ;;
                Contents/MacOS/*) _sbc_qt_framework_present "$value" "$app" "$rel" ;;
            esac
            case "$value" in
                @rpath/*) deferred+=("$rel"$'\t'"$cmd"$'\t'"$value"); continue ;;
            esac
            _sbc_is_system "$value" && continue
            _sbc_absolute_nonsystem "$value" "$app" "$rel: $cmd" && continue
            target="$(_sbc_expand "$value" "$f" "$app")"
            if [ -z "$target" ]; then
                problems+=("$rel: $cmd $value is neither absolute nor relative to the bundle")
            elif ! _sbc_is_inside "$target" "$app"; then
                _sbc_is_system "$target" ||
                    problems+=("$rel: $cmd $value lies outside the bundle")
            elif [ ! -e "$target" ] && [ "$cmd" != LC_LOAD_WEAK_DYLIB ]; then
                problems+=("$rel: $cmd $value names a file the bundle does not hold")
            fi
        done <<< "$commands"
    done

    # Second pass: an @rpath load must be found through a search path
    # inside the bundle. Every LC_RPATH of the bundle is tried, not only the
    # loading file's chain, so this can pass what dyld would miss but never
    # refuses what dyld would find.
    local entry name found r
    for entry in ${deferred[@]+"${deferred[@]}"}; do
        IFS=$'\t' read -r rel cmd value <<< "$entry"
        name="${value#@rpath/}"
        found=""
        for r in ${rpaths[@]+"${rpaths[@]}"}; do
            if [ -e "$r/$name" ]; then found=1; break; fi
        done
        if [ -z "$found" ] && [ "$cmd" != LC_LOAD_WEAK_DYLIB ]; then
            problems+=("$rel: $cmd $value is not found through any LC_RPATH inside the bundle")
        fi
    done

    if [ "${#problems[@]}" -eq 0 ]; then
        echo "  self-contained: ${#machos[@]} Mach-O files load only from the bundle and the system, $((kb / 1024)) MB"
        return 0
    fi
    echo "  NOT SELF-CONTAINED: $given would not start on a Mac without this build's Craft root" >&2
    local p shown=0
    for p in "${problems[@]}"; do
        shown=$((shown + 1))
        if [ "$shown" -gt 40 ]; then
            echo "    ... and $(( ${#problems[@]} - 40 )) more" >&2; break
        fi
        echo "    $p" >&2
    done
    return 1
}

# The variables through which a run could borrow libraries from outside the
# bundle, and so pass for a bundle that is not self-contained.
seabass_clear_dyld_env() {
    unset DYLD_LIBRARY_PATH DYLD_FRAMEWORK_PATH DYLD_FALLBACK_LIBRARY_PATH \
        DYLD_FALLBACK_FRAMEWORK_PATH DYLD_INSERT_LIBRARIES DYLD_VERSIONED_LIBRARY_PATH \
        DYLD_VERSIONED_FRAMEWORK_PATH DYLD_ROOT_PATH
}
