#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# The release train: cutting a train's branch, and tagging releases on it.
# See docs/releasing.md for the whole of it.
#
#   tools/release.sh cut 0.7.9              # what cutting the 0.8 train would do
#   tools/release.sh cut 0.7.9 --go         # cut it
#   tools/release.sh alpha 0.7.9            # what tagging would do
#   tools/release.sh alpha 0.7.9 --go       # tag it
#   tools/release.sh stable 0.8.1 --go --no-tests
#
# The numbers:
#
#   X.Y.8          master between trains: in development, never released
#   X.Y.9, .10 ... alphas and betas of X.(Y+1).0 -- the channel says which
#   X.(Y+1).0      the first stable release of the train
#   X.(Y+1).1-7    its point releases
#
# All of one train comes from its branch, Seabass/X.(Y+1). "cut" makes it
# from master, as Invent has it, with the train's first pre-release number
# committed on the branch only, and in the same run moves master to
# X.(Y+1).8 -- so master and the branch never both claim the release, and
# a build from master cannot call itself one. The branch is stabilized
# where it is and never merged back: fixes are made on it and
# cherry-picked to master.
#
# Tagging is separate, and later: a branch is cut, tested by hand (CI can
# build packages from a Seabass/* branch on request, see docs/releasing.md),
# and only then tagged. A tag is never moved: if a package does not build,
# retry the job, or fix it on the branch and release the next number.
#
# The version lives in CMakeLists.txt and nowhere else. This refuses to
# tag a tree that says something different from the version asked for,
# because packages, installers and the app's own update check all read it
# and all have to agree.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/release-version.sh"

action="${1:-}"
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

usage() {
    echo "usage: $0 cut <X.Y.9> [--go]" >&2
    echo "       $0 <alpha|beta|stable> <X.Y.Z> [--go] [--no-tests]" >&2
    exit 2
}
case "$action" in
    cut|alpha|beta|stable) ;;
    *) usage ;;
esac
case "$version" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "$version is not a X.Y.Z version" >&2; exit 2 ;;
esac

major="${version%%.*}"
minor="${version#*.}"
minor="${minor%%.*}"
patch="${version##*.}"
if [ "$patch" -eq 8 ]; then
    echo "$version is a development version: X.Y.8 is what master carries between trains," >&2
    echo "and it is never released." >&2
    exit 2
elif [ "$patch" -ge 9 ]; then
    train="$major.$((minor + 1))"
else
    train="$major.$minor"
fi
branch="Seabass/$train"

say() { printf '%s\n' "$*"; }
step() { if [ "$go" -eq 1 ]; then say "== $*"; else say "would: $*"; fi; }
run() { if [ "$go" -eq 1 ]; then "$@"; else say "       $*"; fi; }

if [ -n "$(git -C "$root" status --porcelain)" ]; then
    echo "working tree is not clean: commit or stash first" >&2
    git -C "$root" status --short >&2
    exit 1
fi
current="$(git -C "$root" rev-parse --abbrev-ref HEAD)"

# ======================================================================
# cut: the train's branch, from master
# ======================================================================
if [ "$action" = cut ]; then
    if [ "$patch" -ne 9 ]; then
        echo "a train is cut at its first pre-release, X.Y.9; $version is not one." >&2
        exit 2
    fi
    if git -C "$root" rev-parse -q --verify "refs/heads/$branch" >/dev/null \
        || git -C "$root" ls-remote --exit-code --heads origin "$branch" >/dev/null 2>&1; then
        echo "$branch exists already: the $train train was cut before." >&2
        exit 1
    fi
    if [ "$current" != master ]; then
        echo "cut $branch from master: you are on $current." >&2
        exit 1
    fi
    # As Invent has it: the branch and master are both pushed there, and a
    # train cut from anything else would leave out work other people have
    # already pushed, or be refused.
    git -C "$root" fetch -q origin master
    if [ "$(git -C "$root" rev-parse HEAD)" != "$(git -C "$root" rev-parse origin/master)" ]; then
        echo "master here is not Invent's master ($(git -C "$root" rev-parse --short origin/master))." >&2
        echo "Bring it level first, so the train starts from what everyone has." >&2
        exit 1
    fi
    development="$train.8"

    step "cut $branch from master at $(git -C "$root" rev-parse --short HEAD)"
    run git -C "$root" checkout -q -b "$branch"
    step "commit $version on $branch (only there)"
    run seabass_set_project_version "$root" "$version"
    run git -C "$root" commit -q -am "Seabass $version: the $train train is cut"

    # Left for later, master and the branch both say $version, and a build
    # from master calls itself the release it is not.
    step "move master to $development: open for features again"
    run git -C "$root" checkout -q master
    run seabass_set_project_version "$root" "$development"
    run git -C "$root" commit -q -am "Seabass $development: master is open for features again"

    step "push $branch and master to origin (Invent), then to GitHub"
    run git -C "$root" push origin "$branch" master
    run git -C "$root" push seabass "$branch" master
    run git -C "$root" checkout -q "$branch"

    if [ "$go" -eq 0 ]; then
        say ""
        say "Nothing was done. Add --go to run it."
        exit 0
    fi
    say ""
    say "$branch is cut and says $version; master says $development. Nothing is tagged."
    say "Test the branch, then tag it from there:"
    say "  tools/release.sh <alpha|beta> $version --go"
    exit 0
fi

# ======================================================================
# tag: a release, from its train's branch
# ======================================================================
channel="$action"
tag="releases/$channel/$version"
if [ "$patch" -ge 9 ] && [ "$channel" = stable ]; then
    echo "$version is a pre-release of $train.0 (.9 and up are alphas and betas)." >&2
    echo "The stable releases of that train are $train.0 to $train.7." >&2
    exit 2
fi
if [ "$patch" -le 7 ] && [ "$channel" != stable ]; then
    echo "$version is a stable version of the $train train ($train.0 to $train.7)." >&2
    echo "Its alphas and betas are numbered .9 and up on the version before it." >&2
    exit 2
fi

declared="$(seabass_project_version "$root")"
if [ "$declared" != "$version" ]; then
    echo "CMakeLists.txt says $declared, you asked for $version." >&2
    echo "Edit project(seabass VERSION $version ...), commit, then run this again." >&2
    exit 1
fi

if git -C "$root" rev-parse -q --verify "refs/tags/$tag" >/dev/null \
    || git -C "$root" ls-remote --exit-code --tags origin "$tag" >/dev/null 2>&1; then
    echo "$tag exists already. A released version is never re-tagged:" >&2
    echo "fix it on $branch and release the next number instead." >&2
    exit 1
fi

if ! git -C "$root" rev-parse -q --verify "refs/heads/$branch" >/dev/null; then
    echo "no $branch: cut the $train train first," >&2
    echo "  tools/release.sh cut $major.$(( ${train#*.} - 1 )).9 --go" >&2
    exit 1
fi
if [ "$current" != "$branch" ]; then
    echo "$version is tagged from $branch; check it out and run this from there." >&2
    echo "  git checkout $branch" >&2
    exit 1
fi

# ---- the AppStream history ---------------------------------------------
# Software centres show the release list in the metainfo file as the
# version history, so the release goes in before the tag and the tagged
# tree lists itself. Committed only when the file changed: re-running a
# release that failed later on must not stack up empty commits.
metainfo="src/gui/org.kde.seabass.metainfo.xml"
step "record $version ($channel) in $metainfo"
if [ "$go" -eq 1 ]; then
    python3 "$here/appstream-release.py" "$root/$metainfo" "$version" "$channel"
    if ! git -C "$root" diff --quiet -- "$metainfo"; then
        git -C "$root" commit -q -m "Seabass $version: recorded in the AppStream metadata" -- "$metainfo"
    fi
fi

# ---- the gate ----------------------------------------------------------
# The whole suite, integration label included. CI runs it too, on the tag,
# but finding out here costs minutes and finding out there costs a
# release number.
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
