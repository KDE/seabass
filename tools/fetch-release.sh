#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Brings a release's packages down from Invent CI so they can be checked
# on real machines before anybody is offered them.
#
#   tools/fetch-release.sh beta 0.2.1 --watch    # wait for CI, then fetch
#   tools/fetch-release.sh beta 0.2.1            # fetch what is there now
#
# They land in ~/Seabass/releases/<channel>/ as
#   seabass-<version>_<channel>_<linux|windows|macos>.<extension>
# which is the same name and shape the website serves them under, so
# publishing is a copy and not a rename.
#
# Invent is public and so are its artifacts: no token for the ordinary
# case. Set SEABASS_INVENT_TOKEN for a job whose artifacts are not.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/release-version.sh"

API="https://invent.kde.org/api/v4"
PROJECT="multimedia%2Fseabass"
DEST_ROOT="${SEABASS_RELEASES_DIR:-$HOME/Seabass/releases}"

channel="${1:-}"
version="${2:-}"
watch=0
[ "${3:-}" = "--watch" ] && watch=1

case "$channel" in
    alpha|beta|stable) ;;
    *) echo "usage: $0 <alpha|beta|stable> <X.Y.Z> [--watch]" >&2; exit 2 ;;
esac
[ -n "$version" ] || { echo "usage: $0 <alpha|beta|stable> <X.Y.Z> [--watch]" >&2; exit 2; }

tag="releases/$channel/$version"
dest="$DEST_ROOT/$channel"
mkdir -p "$dest"

auth=()
[ -z "${SEABASS_INVENT_TOKEN:-}" ] || auth=(--header "PRIVATE-TOKEN: $SEABASS_INVENT_TOKEN")

api() { curl -fsS "${auth[@]}" "$@"; }

pipeline_id() {
    api "$API/projects/$PROJECT/pipelines?ref=$(printf '%s' "$tag" | jq -sRr @uri)&per_page=1" \
        | jq -r '.[0].id // empty'
}

pipeline_status() {  # <id>
    api "$API/projects/$PROJECT/pipelines/$1" | jq -r '.status'
}

id="$(pipeline_id || true)"
if [ -z "$id" ]; then
    echo "no pipeline for $tag yet." >&2
    echo "Invent starts one when the tag is pushed; tools/release.sh does that." >&2
    exit 1
fi
echo "pipeline $id for $tag: https://invent.kde.org/multimedia/seabass/-/pipelines/$id"

if [ "$watch" -eq 1 ]; then
    # Slowly. A full lane is tens of minutes and there is nothing to do
    # but wait; a tight loop only annoys the server.
    while :; do
        state="$(pipeline_status "$id")"
        printf '%s  %s\n' "$(date +%H:%M:%S)" "$state"
        case "$state" in
            success|failed|canceled|skipped) break ;;
        esac
        sleep 60
    done
    if [ "$state" != "success" ]; then
        echo "pipeline $state: nothing worth downloading. Read the jobs before re-tagging." >&2
        exit 1
    fi
fi

# Which job carries which platform's package. A job that is not there --
# no Windows runner registered, say -- is reported, not silently skipped:
# a release missing a platform must be a decision, never an oversight.
#
# The names are Invent's, exactly. Two of the three here were wrong for
# as long as this script existed and nobody found out, because it had
# never been run against a real tag: "windows:package" is the MSYS2 job
# that no runner picks up, and the macOS job is craft_macos_qt6_arm64,
# not craft_macos_arm64_qt6. Both reported the platform missing, which
# reads exactly like a job that did not run.
want_linux="linux:package"
# The Craft job, not the MSYS2 windows:package: that chain has no runner
# (docs/releasing.md, "Packages before the tag").
want_windows="craft_windows_qt6_x86_64"
want_macos_arm64="craft_macos_qt6_arm64"
want_macos_x86_64="craft_macos_qt6_x86_64"

jobs="$(api "$API/projects/$PROJECT/pipelines/$id/jobs?per_page=100")"

fetch_job() {  # <job name> <os> <extension>
    local name="$1" os="$2" ext="$3"
    local job_id status
    job_id="$(printf '%s' "$jobs" | jq -r --arg n "$name" '[.[] | select(.name == $n)] | .[0].id // empty')"
    status="$(printf '%s' "$jobs" | jq -r --arg n "$name" '[.[] | select(.name == $n)] | .[0].status // "missing"')"
    if [ -z "$job_id" ]; then
        echo "  $os: no job named $name in this pipeline"
        return 1
    fi
    if [ "$status" != "success" ]; then
        echo "  $os: $name is $status, so there is no package to take"
        return 1
    fi
    local target; target="$dest/$(seabass_package_name "$version" "$channel" "$os" "$ext")"
    local tmp; tmp="$(mktemp -d)"
    if ! curl -fsSL "${auth[@]}" "$API/projects/$PROJECT/jobs/$job_id/artifacts" -o "$tmp/artifacts.zip"; then
        echo "  $os: $name has no artifacts archive"
        rm -rf "$tmp"
        return 1
    fi
    ( cd "$tmp" && unzip -q artifacts.zip )
    # The one file of that kind in the archive. More than one is not a
    # thing to guess at: say so and let a person look.
    local found; found="$(find "$tmp" -type f -name "*.$ext" ! -name "artifacts.zip" | sort)"
    local count; count="$(printf '%s\n' "$found" | grep -c . || true)"
    if [ "$count" != "1" ]; then
        echo "  $os: expected one .$ext in $name's artifacts, found $count"
        printf '%s\n' "$found" | sed 's/^/      /'
        rm -rf "$tmp"
        return 1
    fi
    mv "$found" "$target"
    rm -rf "$tmp"
    printf '  %-8s %s\n' "$os:" "$target"
    printf '           %s\n' "$(sha256sum "$target" | cut -d" " -f1)"
    return 0
}

echo "into $dest:"
missing=0
fetch_job "$want_linux" linux tar.gz || missing=1
fetch_job "$want_windows" windows exe || missing=1
# Both Mac architectures, because the package that gets published is
# neither of them: Craft builds one architecture per root, and an
# arm64-only .dmg mounts on an Intel Mac and refuses to launch. These two
# are the halves; tools/macos-universal-dmg.sh makes the whole, on a Mac.
fetch_job "$want_macos_arm64" macos-arm64 dmg || missing=1
fetch_job "$want_macos_x86_64" macos-x86_64 dmg || missing=1

universal="$dest/$(seabass_package_name "$version" "$channel" macos dmg)"
echo
if [ -f "$universal" ]; then
    echo "macos:   $universal"
    echo "         $(sha256sum "$universal" | cut -d" " -f1)"
else
    # Named as a thing that is owed, not as a thing that failed. The
    # publisher looks for this exact filename and will refuse the release
    # without it, so there is no way to publish the arm64 half by
    # mistake.
    missing=1
    echo "macos:   no universal package yet, and the two above are not it."
    echo "         They are CI's halves, one architecture each, and they are here as"
    echo "         evidence that both build. The package that gets published is merged"
    echo "         on a Mac from two Craft roots, because the check that the two roots"
    echo "         hold the same package versions reads their install.db and a .dmg"
    echo "         does not carry one. See docs/releasing.md, \"A universal macOS"
    echo "         package\", then put the result here as:"
    echo "           $(seabass_package_name "$version" "$channel" macos dmg)"
fi

echo
if [ "$missing" -eq 1 ]; then
    echo "Not every platform has a package yet. Publishing a release that is missing"
    echo "one is a decision to take deliberately, not by not noticing."
    exit 1
fi
echo "All three are here. Install and run each one before publishing:"
echo "  docs/releasing.md, \"Verifying a build\""
