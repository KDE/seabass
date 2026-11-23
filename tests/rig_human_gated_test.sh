#!/bin/bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# tools/rig-human-gated.sh: when a check that needs a person is due.
# Against a scratch git repository, so each case names the commits it
# means. Every case asserts the reason as well as the answer: "due" for
# the wrong reason would pass a case whose own reason had stopped working.
#
# SEABASS_HUMAN_GATED names the file to test; the one in tools/ by default.
set -u
set -o pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="${SEABASS_HUMAN_GATED:-$here/../tools/rig-human-gated.sh}"
[ -f "$script" ] || { echo "no such file: $script" >&2; exit 1; }
command -v git >/dev/null 2>&1 || { echo "git is not on PATH, and the rule reads history with it" >&2; exit 1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/seabass-human-gated.XXXXXX")" || exit 1
trap 'rm -rf "$work"' EXIT

repo="$work/repo"
export RIG_HUMAN_REPO="$repo" RIG_HUMAN_STATE="$work/state"
unset RIG_HUMAN_CHECKS RIG_ONLY
# shellcheck source=/dev/null
. "$script"

g() { git -C "$repo" -c user.name=rig -c user.email=rig@example.invalid -c commit.gpgsign=false "$@"; }
commit_file() {  # path, text
    mkdir -p "$repo/$(dirname "$1")"
    printf '%s\n' "$2" > "$repo/$1"
    g add -- "$1" && g commit -q -m "$1: $2"
}

failed=0
# expect <case> due|notdue <substring of the reason> <check>
expect() {
    local name="$1" want="$2" reason="$3" check="$4"
    local said rc=0
    said="$(rig_human_gated_due "$check")" || rc=1
    local got=due
    [ $rc -eq 1 ] && got=notdue
    if [ "$got" != "$want" ] || [[ "$said" != *"$reason"* ]]; then
        echo "FAIL $name: wanted $want with \"$reason\", got $got: $said"
        failed=1
    else
        echo "ok   $name"
    fi
}

mkdir -p "$repo"
git -C "$repo" init -q
commit_file README.md one
commit_file src/infrastructure/media/windows_usb_formatter.cpp one
commit_file src/infrastructure/backup/stick_space.cpp one
commit_file tools/rig-human-gated.sh one

expect "nothing recorded" due "no recorded pass" D2-format-stick-A

rig_human_gated_passed D2-format-stick-A || { echo "FAIL could not record a pass"; failed=1; }
rig_human_gated_passed P3-pull-mid-save || { echo "FAIL could not record a pass"; failed=1; }
first="$(g rev-parse HEAD)"
[ "$(head -n 1 "$RIG_HUMAN_STATE/D2-format-stick-A")" = "$first" ] \
    || { echo "FAIL the recorded pass is not the commit checked out"; failed=1; }
expect "passed at this very commit" notdue "nothing it tests changed since ${first:0:12}" D2-format-stick-A

commit_file src/domain/duplicate_cue_shift.cpp one
expect "a change elsewhere" notdue "nothing it tests changed" D2-format-stick-A
expect "a change elsewhere, the save path" notdue "nothing it tests changed" P3-pull-mid-save

RIG_HUMAN_CHECKS=all expect "a release's round" due "RIG_HUMAN_CHECKS=all" D2-format-stick-A
RIG_HUMAN_CHECKS=none expect "a value that is no value" due "neither 'due' nor 'all'" D2-format-stick-A
RIG_ONLY="F4-fill,D2-format-stick-A" expect "named by RIG_ONLY" due "RIG_ONLY names it" D2-format-stick-A
RIG_ONLY="D2-format-stick-A-more" expect "RIG_ONLY names another check" notdue "nothing it tests changed" D2-format-stick-A

commit_file src/infrastructure/media/windows_usb_formatter.cpp two
expect "the formatter changed" due "src/infrastructure/media/windows_usb_formatter.cpp" D2-format-stick-A
expect "the formatter changed, the save path did not" notdue "nothing it tests changed" P3-pull-mid-save

commit_file src/infrastructure/backup/stick_space.cpp two
expect "a file under a listed folder changed" due "src/infrastructure/backup/stick_space.cpp" P3-pull-mid-save

# A pass at the new commit clears both.
rig_human_gated_passed D2-format-stick-A && rig_human_gated_passed P3-pull-mid-save
expect "passed again after the change" notdue "nothing it tests changed" D2-format-stick-A

commit_file tools/rig-human-gated.sh two
expect "the rule itself changed" due "tools/rig-human-gated.sh" D2-format-stick-A
rig_human_gated_passed D2-format-stick-A

# A change no commit holds: what is under test is not a commit.
printf 'three\n' > "$repo/README.md"
expect "an uncommitted change" due "changes no commit holds" D2-format-stick-A
if rig_human_gated_passed D2-format-stick-A 2>/dev/null; then
    echo "FAIL a pass was recorded on a checkout with uncommitted changes"
    failed=1
else
    echo "ok   no pass is recorded on a checkout with uncommitted changes"
fi
g checkout -q -- README.md

# A recorded commit that this history does not hold (a rebased branch).
main="$(g symbolic-ref --short HEAD)"
g checkout -q --orphan elsewhere
commit_file other.txt one
stray="$(g rev-parse HEAD)"
g checkout -q "$main"
printf '%s\n' "$stray" > "$RIG_HUMAN_STATE/D2-format-stick-A"
expect "a recorded commit outside this history" due "is not in this one's history" D2-format-stick-A
printf '%s\n' "0123456789012345678901234567890123456789" > "$RIG_HUMAN_STATE/D2-format-stick-A"
expect "a recorded commit that does not exist" due "is not in this one's history" D2-format-stick-A

# A record that names a commit some other way resolves today and means
# nothing tomorrow.
for ref in HEAD "$main" "${first:0:7}" ""; do
    printf '%s\n' "$ref" > "$RIG_HUMAN_STATE/D2-format-stick-A"
    if [ -n "$ref" ]; then
        expect "a record that says '$ref'" due "not a whole commit id" D2-format-stick-A
    else
        expect "a record with an empty first line" due "no recorded pass" D2-format-stick-A
    fi
done

# A release tag is no pass.
rm -f "$RIG_HUMAN_STATE/D2-format-stick-A"
g -c tag.gpgSign=false tag releases/stable/9.9.9
expect "nothing recorded, at a release tag" due "no recorded pass" D2-format-stick-A

# A new file under the check's own paths, tracked by nothing; one
# elsewhere is not its business.
rig_human_gated_passed D2-format-stick-A
printf 'new\n' > "$repo/untracked-elsewhere.txt"
expect "an untracked file elsewhere" notdue "nothing it tests changed" D2-format-stick-A
printf 'new\n' > "$repo/src/infrastructure/media/haiku_usb_formatter.cpp"
expect "an untracked file it tests" due "changes no commit holds" D2-format-stick-A
rm -f "$repo/src/infrastructure/media/haiku_usb_formatter.cpp" "$repo/untracked-elsewhere.txt"

# The stick pull is named through its bundle.
rig_human_gated_passed live-stick-pulled
expect "the stick pull, not named" notdue "nothing it tests changed" live-stick-pulled
RIG_ONLY="W1-W3-W4-F1-F2-F3-live" expect "the stick pull, its bundle named" due "RIG_ONLY names it" live-stick-pulled
RIG_ONLY="W1-W3-W4-F1-F2-F3-live" expect "another check, that bundle named" notdue "nothing it tests changed" D2-format-stick-A

# A checkout that moved under a round records nothing.
if RIG_HUMAN_ROUND_COMMIT="$first" rig_human_gated_passed D2-format-stick-A 2>/dev/null; then
    echo "FAIL a pass was recorded for a checkout that moved during the round"
    failed=1
else
    echo "ok   no pass is recorded for a checkout that moved during the round"
fi

# A manual check the rule has no paths for is due on any change at all.
rig_human_gated_passed X9-some-new-manual-check "$first"
expect "a check without paths of its own" due "file(s) it tests changed" X9-some-new-manual-check

RIG_HUMAN_REPO="$work/no-such-checkout" expect "no git checkout" due "is not a git checkout" D2-format-stick-A

if [ $failed -ne 0 ]; then
    echo "rig_human_gated_test: FAILED"
    exit 1
fi
echo "rig_human_gated_test: all passed"
