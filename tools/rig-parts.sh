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
#   rig_part id PASS|FAIL      one result
#   rig_parts_finish           declared but never reported -> FAIL
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

rig_part() {  # id, PASS|FAIL
    echo "--- $1: $2"
    rig_parts_seen="$rig_parts_seen $1"
    [ -n "${RIG_PARTS:-}" ] || return 0
    printf '%s\t%s\n' "$1" "$2" >> "$RIG_PARTS"
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
