# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# One result line per test, for the rig checks that run several.
#
# A check that runs six tests and reports one verdict tells a reader the
# six failed when one did, and the board cannot say which. With RIG_PARTS
# set to a file, a script writes one "<id>\tPASS|FAIL" line per test into
# it and rig-shakedown.sh puts those in summary.tsv instead of its own
# single line. Without RIG_PARTS every function here is a no-op and the
# script behaves exactly as it did.
#
#   rig_parts_declare id...    every id this script is answerable for
#   rig_part id PASS|FAIL      one result (PENDING: a manual check not done)
#   rig_parts_finish           declared but never reported -> FAIL
#
# Automatic and manual. The board shows the checks a round runs on its own
# on one tab and those that need a person on another, and each can reach
# 100% (Sebastian, 2026-09-28). A line for a check that needs a person on
# this platform carries a third column, "manual"; a line without one is
# automatic, which is every summary written before this. rig_manual_ids()
# (rig-platform.sh) is the one list of which checks those are.
#
# A manual check nobody has done yet is PENDING, not FAIL: the manual tab
# shows it as to do. That applies to the manual set only -- an automatic
# check that skipped or did not run is a FAIL, as it always was.
#
# Declaring up front is the point: a script that dies halfway still
# accounts for the tests it never reached, as failures. Left unsaid, the
# board keeps whatever it said about them last time, which is how a
# test that stopped running goes on reading green.

rig_parts_declared=""
rig_parts_seen=""

rig_parts_declare() {
    rig_parts_declared="$rig_parts_declared $*"
}

# Whether a check needs a person here: rig_manual_ids() in rig-platform.sh
# decides. rig-clones.sh sources this file without rig-platform.sh, and
# none of its checks is manual anywhere.
rig_is_manual() {  # id
    declare -F rig_manual_ids >/dev/null || return 1
    [[ " $(rig_manual_ids) " == *" $1 "* ]]
}

# One summary line: "<id>\t<verdict>", plus "\tmanual" for a manual check.
rig_summary_line() {  # id, PASS|FAIL|PENDING
    if rig_is_manual "$1"; then
        printf '%s\t%s\tmanual\n' "$1" "$2"
    else
        printf '%s\t%s\n' "$1" "$2"
    fi
}

rig_part() {  # id, PASS|FAIL|PENDING
    echo "--- $1: $2"
    rig_parts_seen="$rig_parts_seen $1"
    [ -n "${RIG_PARTS:-}" ] || return 0
    rig_summary_line "$1" "$2" >> "$RIG_PARTS"
}

# Convenience: rig_part_rc <id> <exit status>
rig_part_rc() {
    if [ "$2" -eq 0 ]; then rig_part "$1" PASS; else rig_part "$1" FAIL; fi
}

rig_parts_finish() {
    local id
    for id in $rig_parts_declared; do
        case " $rig_parts_seen " in
            *" $id "*) continue ;;
        esac
        rig_part "$id" FAIL
        echo "    (it never ran, and a test that never ran has proved nothing)"
    done
}
