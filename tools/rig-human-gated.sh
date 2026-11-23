#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# When a check that needs a person is due.
#
# The checks of rig_manual_ids() (rig-platform.sh) cost a person at the
# machine: a UAC click, a hand on the stick, a player. In the month to
# 2026-11-23 not one of them found a defect, and most rounds changed
# nothing they test. So a round asks for one only when it could say
# something (Sebastian, 2026-11-23):
#
# - the code it tests changed since the commit it last passed at on this
#   machine (the paths below, per check), or
# - the round is a release's (RIG_HUMAN_CHECKS=all), or
# - RIG_ONLY names it (for the stick pull, the bundle it runs in), or
# - the rig cannot tell: no git, no recorded pass, a record that is not
#   one whole commit id, a recorded commit that is not an ancestor of
#   this one, changes no commit holds. Unsure is due.
#
# A release tag is no pass: a tag says a release was cut, not that
# anybody pulled a stick. What is recorded is the commit checked out,
# which the rig takes the build under test to be made from, as it does
# everywhere else.
#
# A check that is not due is written NOTDUE to the summary, with the
# commit it stands on, and the board shows it as "not due": neither done
# nor to do. It is never written for a check a machine runs on its own;
# those run every round, and one that did not run is a FAIL as before.
#
# Sourced by rig-shakedown.sh and tests/qml-live/run-live.sh, after
# rig-platform.sh. Run directly, it records a by-hand check's pass, which
# no script sees:
#
#   tools/rig-human-gated.sh passed P3-pull-mid-save [commit]
#   tools/rig-human-gated.sh due P3-pull-mid-save      (says why, exit 0 when due)

# The paths whose change makes a check due, as git pathspecs from the
# repository root. This file is in every list: a change to the lists
# themselves must not be the thing that hides a check.
rig_human_gated_paths() {  # check
    echo "tools/rig-human-gated.sh tools/rig-platform.sh"
    case "$1" in
        D2-format-stick-A)
            echo "src/application/ports/usb_formatter.hpp src/application/use_cases/format_usb_stick.hpp"
            echo "src/domain/usb_filesystem.cpp src/domain/usb_filesystem.hpp"
            echo "src/gui/format_usb_controller.cpp src/gui/format_usb_controller.hpp src/gui/qml/FormatUsbPage.qml"
            echo "src/infrastructure/media/*usb_formatter* tools/rig_format.cpp"
            # What rig_format finds the stick with, and Windows' elevation.
            echo "src/application/ports/removable_media_locator.hpp src/infrastructure/media/*removable_media_locator*"
            echo "src/infrastructure/media/media_factory.cpp src/infrastructure/media/media_factory.hpp"
            ;;
        live-stick-pulled)
            echo "src/application/ports/removable_media_* src/application/mount_unless_mounted.hpp"
            echo "src/application/stick_presence_diff.hpp src/application/stick_identity.hpp"
            echo "src/infrastructure/media/*removable_media* src/infrastructure/media/*media_mounter*"
            echo "src/gui/library_access.cpp src/gui/library_access.hpp tests/qml-live"
            echo "src/infrastructure/media/media_factory.cpp src/infrastructure/media/media_factory.hpp"
            echo "src/gui/qml/common/EjectHeldDialog.qml"
            ;;
        H1-filesystem-repair)
            echo "src/infrastructure/media/filesystem_health.cpp src/infrastructure/media/filesystem_health.hpp"
            echo "src/domain/filesystem_compatibility.cpp src/domain/filesystem_compatibility.hpp"
            echo "src/cli/damage_filesystem.cpp src/cli/damage_filesystem.hpp tools/rig_fs_repair.cpp"
            echo "src/infrastructure/process"
            ;;
        P3-pull-mid-save)
            # What a save does to a stick, and what reads a stick a save
            # did not finish.
            echo "src/infrastructure/backup src/infrastructure/durable_file_write.cpp"
            echo "src/infrastructure/durable_file_write.hpp src/infrastructure/sqlite_pending_journal.hpp"
            echo "src/infrastructure/engine/engine_pending_journals.hpp src/infrastructure/file_placement.cpp"
            echo "src/infrastructure/file_placement.hpp src/infrastructure/bulk_write_strategy.cpp"
            echo "src/infrastructure/bulk_write_strategy.hpp"
            echo "src/gui/edit/save_context.cpp src/gui/edit/save_context.hpp src/gui/edit/save_loop.cpp"
            echo "src/gui/edit/save_loop.hpp src/gui/edit/library_edit_session.cpp"
            echo "src/gui/edit/library_edit_session.hpp src/gui/edit/format_write_session.cpp"
            echo "src/gui/edit/format_write_session.hpp src/gui/edit/direct_write_hold.cpp"
            echo "src/gui/edit/direct_write_hold.hpp tools/rig-interrupted-save.sh"
            echo "src/infrastructure/stick_backup"
            # The writers themselves: the order they write a stick in is
            # what a pull cuts through.
            echo "src/infrastructure/engine src/infrastructure/onelibrary src/infrastructure/rekordbox"
            ;;
        P1-player-hardware)
            # Everything that writes what a player reads.
            echo "src/infrastructure/engine src/infrastructure/onelibrary src/infrastructure/rekordbox"
            echo "third_party"
            ;;
        P4-player-first-load)
            echo "src/gui/playback_controller.cpp src/gui/playback_controller.hpp"
            echo "src/gui/media_controller.cpp src/gui/media_controller.hpp"
            echo "src/gui/mpris_service.cpp src/gui/mpris_service.hpp"
            echo "src/gui/qml/Player*.qml src/infrastructure/audio/*playback*"
            ;;
        *)
            # A manual check this file has no paths for is due every round.
            echo "."
            ;;
    esac
}

rig_human_gated_state_dir() {
    echo "${RIG_HUMAN_STATE:-$HOME/Seabass/e2e/human-gated}"
}

rig_human_gated_repo() {
    echo "${RIG_HUMAN_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
}

# The commit a check last passed at on this machine, as recorded: the
# first line of its file. Empty when there is none.
rig_human_gated_baseline() {  # check
    local file; file="$(rig_human_gated_state_dir)/$1"
    [ -s "$file" ] || return 0
    head -n 1 "$file" | tr -d '[:space:]'
}

# The round's commit: the one checked out when the rig started
# (RIG_HUMAN_ROUND_COMMIT, set by rig-shakedown.sh), else the one checked
# out now. Empty when there is no git checkout.
rig_human_gated_head() {
    local repo; repo="$(rig_human_gated_repo)"
    git -C "$repo" rev-parse --verify --quiet 'HEAD^{commit}' 2>/dev/null
}

# Changes no commit holds: any change to a tracked file, and any new file
# under the check's own paths (a new test or QML file is globbed into the
# build without being tracked).
rig_human_gated_dirty() {  # check
    local repo; repo="$(rig_human_gated_repo)"
    [ -n "$(git -C "$repo" status --porcelain --untracked-files=no 2>/dev/null)" ] && return 0
    local untracked
    set -f
    # shellcheck disable=SC2046  # the pathspecs are words on purpose
    untracked="$(git -C "$repo" status --porcelain --untracked-files=normal -- $(rig_human_gated_paths "$1") 2>/dev/null)"
    set +f
    [ -n "$untracked" ]
}

# rig_human_gated_due <check>: exit 0 when the round must ask for it, 1
# when it is not due. One line on stdout says why either way.
rig_human_gated_due() {  # check
    local check="$1"
    local repo; repo="$(rig_human_gated_repo)"
    if [ "${RIG_HUMAN_CHECKS:-due}" = "all" ]; then
        echo "due: RIG_HUMAN_CHECKS=all, as for a release's round"
        return 0
    fi
    if [ "${RIG_HUMAN_CHECKS:-due}" != "due" ]; then
        echo "due: RIG_HUMAN_CHECKS is '${RIG_HUMAN_CHECKS}', which is neither 'due' nor 'all'"
        return 0
    fi
    local only; only=" $(printf '%s' "${RIG_ONLY:-}" | tr ',\t\n' '   ') "
    # The stick pull is one test of the live bundle, which is what
    # RIG_ONLY can name.
    if [[ "$only" == *" $check "* ]] \
        || { [ "$check" = live-stick-pulled ] && [[ "$only" == *" W1-W3-W4-F1-F2-F3-live "* ]]; }; then
        echo "due: RIG_ONLY names it"
        return 0
    fi
    local head
    if ! head="$(rig_human_gated_head)" || [ -z "$head" ]; then
        echo "due: $repo is not a git checkout, so what changed cannot be known"
        return 0
    fi
    if rig_human_gated_dirty "$check"; then
        echo "due: the checkout has changes no commit holds, so what is under test cannot be known"
        return 0
    fi
    local base; base="$(rig_human_gated_baseline "$check")"
    if [ -z "$base" ]; then
        echo "due: it has no recorded pass on this machine"
        return 0
    fi
    # One whole commit id and nothing else: "HEAD", a branch name or a
    # short id would resolve, and to whatever is checked out today.
    if ! [[ "$base" =~ ^[0-9a-f]{40}([0-9a-f]{24})?$ ]]; then
        echo "due: its recorded pass is not a whole commit id ('${base:0:20}')"
        return 0
    fi
    if ! git -C "$repo" merge-base --is-ancestor "$base" "$head" 2>/dev/null; then
        echo "due: the commit it last passed at (${base:0:12}) is not in this one's history"
        return 0
    fi
    local changed
    # shellcheck disable=SC2046  # the pathspecs are words on purpose
    set -f
    if ! changed="$(git -C "$repo" diff --name-only "$base" "$head" -- $(rig_human_gated_paths "$check") 2>/dev/null)"; then
        set +f
        echo "due: git could not compare ${base:0:12} with ${head:0:12}"
        return 0
    fi
    set +f
    if [ -n "$changed" ]; then
        echo "due: $(printf '%s\n' "$changed" | grep -c .) file(s) it tests changed since ${base:0:12}, the first $(printf '%s\n' "$changed" | head -n 1)"
        return 0
    fi
    echo "not due: nothing it tests changed since ${base:0:12}, where it stands"
    return 1
}

# Records that a check passed at a commit (default: the one checked out),
# so later rounds compare with it.
rig_human_gated_passed() {  # check [commit]
    local check="$1"
    local repo; repo="$(rig_human_gated_repo)"
    local commit
    if ! commit="$(git -C "$repo" rev-parse --verify --quiet "${2:-HEAD}^{commit}" 2>/dev/null)" || [ -z "$commit" ]; then
        echo "rig-human-gated: no commit '${2:-HEAD}' in $repo; nothing recorded" >&2
        return 1
    fi
    if [ -z "${2:-}" ]; then
        # A pass on a checkout with uncommitted changes is a pass of
        # something no commit names; recording it would let a later round
        # stand on it. The same for a checkout that moved under a round.
        if rig_human_gated_dirty "$check"; then
            echo "rig-human-gated: the checkout has uncommitted changes; nothing recorded for $check" >&2
            return 1
        fi
        if [ -n "${RIG_HUMAN_ROUND_COMMIT:-}" ] && [ "$commit" != "$RIG_HUMAN_ROUND_COMMIT" ]; then
            echo "rig-human-gated: the checkout moved during the round (${RIG_HUMAN_ROUND_COMMIT:0:12} to ${commit:0:12}); nothing recorded for $check" >&2
            return 1
        fi
    fi
    local dir; dir="$(rig_human_gated_state_dir)"
    mkdir -p "$dir" || return 1
    printf '%s\n%s\n' "$commit" "$(date -Iseconds 2>/dev/null || date)" > "$dir/$check.new" \
        && mv "$dir/$check.new" "$dir/$check"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    set -u
    here="$(cd "$(dirname "$0")" && pwd)"
    . "$here/rig-platform.sh"
    command="${1:-}"
    check="${2:-}"
    if [ -z "$check" ] || [[ " $(rig_manual_ids) " != *" $check "* ]]; then
        echo "usage: $0 passed|due <check> [commit]" >&2
        echo "the checks that need a person here: $(rig_manual_ids)" >&2
        exit 2
    fi
    case "$command" in
        passed)
            rig_human_gated_passed "$check" "${3:-}" || exit 1
            echo "$check: passed at $(head -n 1 "$(rig_human_gated_state_dir)/$check")"
            ;;
        due)
            rig_human_gated_due "$check"
            ;;
        *)
            echo "usage: $0 passed|due <check> [commit]" >&2
            exit 2
            ;;
    esac
fi
