#!/bin/bash
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# One round of putting a created Engine Library in front of real hardware.
#
#   engine-hardware-round.sh install <candidate dir> <stick root>
#   engine-hardware-round.sh verdict <candidate dir> <stick root>
#
# <candidate dir> holds an "Engine Library" folder, typically one
# make_engine_library produced.
#
# Exists because the interesting half of this test is not visible on the
# player. A Prime 4 given a library it will not use does not necessarily
# say so: it can quietly write its own empty database over the one it was
# given, and the deck then looks merely empty rather than rejected. Told
# only "no error, but nothing to browse", we read that as progress when
# the library had in fact been discarded eight minutes earlier. The
# filesystem knew. So the verdict is computed from the stick, not from
# what the screen said: "install" records what went on, "verdict" reports
# whether that is still what is there.
#
# install refuses a candidate whose Information row is not at id 1, and
# refuses one whose track paths climb off the stick -- the two defects
# this project has confirmed a Prime 4 reacts to, the first by calling the
# stick corrupt, the second by listing every track and playing none of
# them ("file unavailable").
#
# install records what it put on the stick, in a note beside the candidate
# folder rather than inside it, and verdict compares against that record.
# Comparing against whatever is staged *now* is wrong: restaging a
# candidate gives it a new identity, and the verdict then reads a stick
# that was simply never reinstalled as one the player wiped. That
# misreading happened, and cost a round.

set -u

fail() { echo "error: $*" >&2; exit 2; }

describe_m_db() {
    # uuid, Information row id, track count -- the identity of a library.
    python3 - "$1" <<'PY'
import sqlite3, sys
try:
    c = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
    uuid, ident = c.execute("SELECT uuid, id FROM Information").fetchone()
    tracks = c.execute("SELECT count(*) FROM Track").fetchone()[0]
    dangling = len(c.execute("PRAGMA foreign_key_check").fetchall())
    print(f"{uuid}\t{ident}\t{tracks}\t{dangling}")
except Exception as e:
    print(f"UNREADABLE\t0\t0\t0")
PY
}

# Every stored path, judged relative to the stick rather than to this
# machine: "Engine Library" is one level down, so a path that still starts
# with ".." after normalising leaves the device. Such a path resolves fine
# here -- the leading "../" run collapses at this filesystem's root and
# lands back on the stick, because that is where it happens to be mounted
# -- and resolves nowhere in a deck.
paths_stay_on_the_stick() {
    python3 - "$1" <<'PY'
import sqlite3, sys, posixpath
c = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
off = total = 0
first = ""
for (path,) in c.execute("SELECT path FROM Track"):
    total += 1
    if posixpath.normpath(posixpath.join("Engine Library", path)).startswith(".."):
        off += 1
        first = first or path
print(f"{off}\t{total}\t{first}")
PY
}

check_candidate() {
    local db="$1/Engine Library/Database2/m.db"
    [ -f "$db" ] || fail "no Engine Library in $1"
    local info; info=$(describe_m_db "$db")
    local ident; ident=$(echo "$info" | cut -f2)
    local tracks; tracks=$(echo "$info" | cut -f3)
    [ "$ident" = "1" ] || fail "this candidate's Information row is at id $ident; Engine expects it at id 1, and a Prime 4 calls anything else corrupt"
    [ "$tracks" != "0" ] || fail "this candidate has no tracks"
    local paths; paths=$(paths_stay_on_the_stick "$db")
    local off; off=$(echo "$paths" | cut -f1)
    if [ "$off" != "0" ]; then
        fail "$off of $(echo "$paths" | cut -f2) track paths climb off the stick, e.g. $(echo "$paths" | cut -f3) -- a player lists every track and plays none of them"
    fi
    echo "$info"
}

# Beside the candidate folder, not inside it, so rebuilding the candidate
# does not erase the record of what the player was actually given.
record_for() {
    echo "$(dirname "$1")/.installed-$(basename "$1").tsv"
}

case "${1:-}" in
install)
    [ $# -eq 3 ] || fail "usage: engine-hardware-round.sh install <candidate dir> <stick root>"
    candidate="$2"; stick="$3"
    [ -d "$stick/PIONEER" ] || [ -d "$stick/Contents" ] || fail "$stick does not look like a stick root"
    info=$(check_candidate "$candidate") || exit 2
    echo "candidate: $(echo "$info" | cut -f3) tracks, uuid $(echo "$info" | cut -f1)"
    if [ -d "$stick/Engine Library" ]; then
        echo "replacing the Engine Library already on $stick"
        rm -rf "$stick/Engine Library" || fail "could not remove the existing Engine Library"
    fi
    cp -a "$candidate/Engine Library" "$stick/" || fail "could not copy the candidate onto the stick"
    sync
    installed=$(describe_m_db "$stick/Engine Library/Database2/m.db")
    [ "$installed" = "$info" ] || fail "what landed on the stick is not what was staged"
    printf '%s\t%s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$info" > "$(record_for "$candidate")"
    echo "installed and flushed. Eject, play it on the player, bring it back, then:"
    echo "  $0 verdict \"$candidate\" \"$stick\""
    ;;
verdict)
    [ $# -eq 3 ] || fail "usage: engine-hardware-round.sh verdict <candidate dir> <stick root>"
    candidate="$2"; stick="$3"
    cdb="$candidate/Engine Library/Database2/m.db"
    sdb="$stick/Engine Library/Database2/m.db"
    [ -f "$cdb" ] || fail "no candidate library at $candidate"
    if [ ! -f "$sdb" ]; then
        echo "VERDICT: REMOVED -- there is no Engine Library on the stick at all"
        exit 1
    fi
    # What was installed, not what happens to be staged now.
    record=$(record_for "$candidate")
    if [ -f "$record" ]; then
        echo "installed $(cut -f1 "$record")"
        want=$(cut -f2- "$record")
    else
        echo "no install record beside this candidate; comparing with what is staged now"
        want=$(describe_m_db "$cdb")
    fi
    got=$(describe_m_db "$sdb")
    echo "staged:  uuid $(echo "$want" | cut -f1), $(echo "$want" | cut -f3) tracks"
    echo "on disk: uuid $(echo "$got" | cut -f1), $(echo "$got" | cut -f3) tracks"
    echo "written: $(find "$stick/Engine Library/Database2" -name '*.db' -printf '%TF %TT %f\n' | sort)"
    if [ "$(echo "$want" | cut -f1)" != "$(echo "$got" | cut -f1)" ]; then
        if [ -f "$record" ]; then
            echo "VERDICT: REPLACED -- the player threw the library away and wrote its own."
            echo "  Nothing was accepted, whatever the screen said."
        else
            echo "VERDICT: UNKNOWN -- what is on the stick is not what is staged here, and"
            echo "  there is no record of an install. Most likely the stick was never"
            echo "  reinstalled after the candidate was rebuilt. Install first, then judge."
        fi
        exit 1
    fi
    if [ "$want" = "$got" ]; then
        echo "VERDICT: KEPT -- the library came back exactly as it went out."
        exit 0
    fi
    echo "VERDICT: KEPT AND EDITED -- same library, the player changed it."
    echo "  That is what a working stick looks like after a set."
    exit 0
    ;;
*)
    echo "usage: engine-hardware-round.sh install|verdict <candidate dir> <stick root>" >&2
    exit 2
    ;;
esac
