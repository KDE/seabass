#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Cuts a release: the branch, the tag, the push, and then it watches CI
# build the packages. See docs/releasing.md for the whole train.
#
#   tools/release.sh alpha 0.2.0            # what it would do
#   tools/release.sh alpha 0.2.0 --go       # do it
#   tools/release.sh beta 0.2.1 --go --no-tests
#
# X.Y.Z, semantic. Every X.Y has a branch release/X.Y, cut from master at
# the .0 and never merged back; X.Y.Z releases are tagged from it, so a
# patch for a shipped release does not have to carry whatever master has
# moved on to.
#
# The version lives in CMakeLists.txt and nowhere else. This refuses to
# tag a tree that says something different from the version asked for,
# because packages, installers and the app's own update check all read it
# and all have to agree.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/release-version.sh"

channel="${1:-}"
version="${2:-}"
shift 2 2>/dev/null || true

go=0
run_tests=1
for arg in "$@"; do
    case "$arg" in
        --go) go=1 ;;
        --no-tests) run_tests=0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

case "$channel" in
    alpha|beta|stable) ;;
    *)
        echo "usage: $0 <alpha|beta|stable> <X.Y.Z> [--go] [--no-tests]" >&2
        exit 2
        ;;
esac
case "$version" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "$version is not a X.Y.Z version" >&2; exit 2 ;;
esac

series="${version%.*}"          # 0.2.1 -> 0.2
patch="${version##*.}"
branch="release/$series"
tag="releases/$channel/$version"

say() { printf '%s\n' "$*"; }
step() { if [ "$go" -eq 1 ]; then say "== $*"; else say "would: $*"; fi; }
run() { if [ "$go" -eq 1 ]; then "$@"; else say "       $*"; fi; }

# ---- the tree has to be exactly what gets released ---------------------
if [ -n "$(git -C "$root" status --porcelain)" ]; then
    echo "working tree is not clean: commit or stash first" >&2
    git -C "$root" status --short >&2
    exit 1
fi

declared="$(seabass_project_version "$root")"
if [ "$declared" != "$version" ]; then
    echo "CMakeLists.txt says $declared, you asked for $version." >&2
    echo "Edit project(seabass VERSION $version ...), commit, then run this again." >&2
    exit 1
fi

if git -C "$root" rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    echo "$tag exists already. A released version is never re-tagged:" >&2
    echo "bump the patch number and release that instead." >&2
    exit 1
fi

# ---- the branch for this X.Y ------------------------------------------
current="$(git -C "$root" rev-parse --abbrev-ref HEAD)"
if git -C "$root" rev-parse -q --verify "refs/heads/$branch" >/dev/null; then
    if [ "$current" != "$branch" ]; then
        echo "$branch exists; check it out and run this from there." >&2
        echo "  git checkout $branch" >&2
        exit 1
    fi
elif [ "$patch" = "0" ]; then
    # The .0 of a series is where the branch comes from, and master is
    # the only sensible place to cut it.
    if [ "$current" != "master" ]; then
        echo "cut $branch from master: you are on $current." >&2
        exit 1
    fi
    step "cut $branch from master"
    run git -C "$root" checkout -b "$branch"
else
    echo "no $branch, and $version is not a .0." >&2
    echo "A patch release is tagged from its series branch; release $series.0 first." >&2
    exit 1
fi

# ---- the gate ----------------------------------------------------------
# The whole suite, integration label included. CI runs it too, on the tag,
# but finding out here costs minutes and finding out there costs a
# release tag that has to be abandoned.
if [ "$run_tests" -eq 1 ]; then
    build="${SEABASS_BUILD_DIR:-$HOME/Seabass/builds/master}"
    step "run the full suite in $build"
    if [ "$go" -eq 1 ]; then
        cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release "-DSEABASS_RELEASE_CHANNEL=$channel" >/dev/null
        cmake --build "$build" -j"$(nproc 2>/dev/null || echo 4)"
        ( cd "$build" && ctest --output-on-failure )
    fi
else
    say "!! skipping the suite (--no-tests). CI still runs it on the tag."
fi

# ---- tag and push ------------------------------------------------------
step "tag $tag"
run git -C "$root" tag -a "$tag" -m "Seabass $version ($channel)"

# Invent first: it is where CI lives, and a tag on GitHub that Invent
# never saw builds nothing.
step "push $branch and $tag to origin (Invent), then to GitHub"
run git -C "$root" push origin "$branch" "$tag"
run git -C "$root" push seabass "$branch" "$tag"

if [ "$go" -eq 0 ]; then
    say ""
    say "Nothing was done. Add --go to run it."
    exit 0
fi

say ""
say "Pushed. CI builds the packages on the tag; watch it with:"
say "  tools/fetch-release.sh $channel $version --watch"
