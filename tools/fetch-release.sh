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
want_linux="linux:package"
want_windows="windows:package"
want_macos="craft_macos_arm64_qt6"

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
fetch_job "$want_macos" macos dmg || missing=1

echo
if [ "$missing" -eq 1 ]; then
    echo "Some platforms have no package. Publishing a release that is missing one"
    echo "is a decision to take deliberately, not by not noticing."
    exit 1
fi
echo "All three are here. Install and run each one before publishing:"
echo "  docs/releasing.md, \"Verifying a build\""
