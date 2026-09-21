#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Release rig: every scripted check in one run, against two TEST sticks and
# two reference backups. See docs/testing.md and the rig's results page.
#
#   tools/rig-shakedown.sh <out dir>
#
# Configured by environment (defaults are the rig machine's):
#   SEABASS_BUILD_DIR   the build to test                    (<repo>/build)
#   RIG_STICK_A         test stick A, gets reference A        (/media/sebas/RV2; macOS /Volumes/VSTICKA)
#   RIG_STICK_B         test stick B, gets reference B        (/media/sebas/A4-128GB; macOS /Volumes/VSTICKB)
#   RIG_DEVICE_B        stick B's partition, for the pull test (/dev/sdc1; macOS: B's device node)
#   RIG_REFERENCE_A     reference backup for A (never written) (~/Seabass/e2e/backups/CORSAIR.zip)
#   RIG_REFERENCE_B     reference backup for B (never written) (~/Seabass/e2e/backups/WHALESHARK2.zip)
#   RIG_REFERENCE_PRINTS  size/mtime/manifest sha256 of both references, as check S4 recorded them
#
# On macOS the sticks may be mounted disk images (see docs/testing.md): the
# rig (tools/rig-platform.sh) sets SEABASS_ACCEPT_DISK_IMAGES=1 so the app
# lists them.
#
# Both sticks are overwritten, several times, and end as exact copies of
# their references. The references are only read. Run it with the sandbox
# profile (SEABASS_HOME, XDG_*) so nothing lands in a real profile.
#
# Each check writes <out dir>/<check>.log and one line "<check> PASS|FAIL"
# to <out dir>/summary.tsv. A failed check does not stop the run, except
# when a stick could not be put into the state the next checks need.
set -u

out="${1:?output directory}"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/rig-platform.sh"
. "$here/rig-parts.sh"

# One round at a time on one machine. The sticks are a single resource
# with no lock of their own, and two rounds sharing them do not collide
# loudly: they restore over each other, fill each other's free space and
# write each other's catalogs, and both summaries come out looking like
# ordinary results. That happened here on 2026-09-21 -- two Linux rounds
# started a minute apart against the same A1 and TESTRIG_2, and the only
# reason anybody noticed was a person reading pgrep. Both rounds had to
# be thrown away.
#
# flock, not a pid file: it is released when the process dies however it
# dies, including a kill -9 or a crash mid-fill, so a stale lock cannot
# block the next round. RIG_NO_LOCK is for running a second rig against
# DIFFERENT sticks deliberately, which is the only case this refuses
# that it should not.
rig_lock="${RIG_LOCK_FILE:-$HOME/Seabass/e2e/.rig-running.lock}"
rig_lock_dir=""
if [ -z "${RIG_NO_LOCK:-}" ]; then
    mkdir -p "$(dirname "$rig_lock")"
    if rig_is_windows; then
        # flock needs a real fd->HANDLE mapping from the same MSYS
        # runtime it was built against. This shell (Git for Windows' own
        # bash) and the flock.exe a full MSYS2 install carries are
        # different runtimes, and handing an inherited fd number across
        # them fails outright -- confirmed directly, "flock: 9: Bad file
        # descriptor" every time, on a machine where flock.exe is right
        # there on disk. mkdir is atomic here too, the same guarantee
        # flock gives on a real fd, and a stale lock (the process that
        # made it is gone) is told apart from a live one with `kill -0`,
        # which -- also confirmed directly, across two separate bash
        # invocations -- resolves an MSYS PID correctly, unlike asking
        # tasklist for that same number: that is the WINPID/PID split
        # `ps aux` already prints two separate columns for.
        rig_lock_dir="$rig_lock.d"
        if [ -d "$rig_lock_dir" ]; then
            held_pid="$(cat "$rig_lock_dir/pid" 2>/dev/null || true)"
            if [ -z "$held_pid" ] || ! kill -0 "$held_pid" 2>/dev/null; then
                rm -rf "$rig_lock_dir"
            fi
        fi
        if ! mkdir "$rig_lock_dir" 2>/dev/null; then
            echo "another shakedown round is already running on this machine:" >&2
            cat "$rig_lock_dir/info" >&2 2>/dev/null || true
            echo >&2
            echo "Two rounds on the same sticks make both meaningless. Wait for it, or set" >&2
            echo "RIG_NO_LOCK=1 if you are deliberately running against different sticks." >&2
            exit 1
        fi
        echo "$$" > "$rig_lock_dir/pid"
        { echo "pid $$"; echo "out $out"; echo "started $(date -Iseconds)"
          echo "sticks ${RIG_STICK_A:-default} ${RIG_STICK_B:-default}"; } > "$rig_lock_dir/info"
    else
        # <> rather than >: opening for write TRUNCATES, and the process that
        # gets refused opens the file before it discovers it cannot lock it.
        # The first version of this printed "another round is already
        # running:" followed by nothing, having just erased the lines it was
        # about to read -- the refusal worked and the one useful thing about
        # it did not.
        exec 9<>"$rig_lock"
        if ! flock -n 9; then
            echo "another shakedown round is already running on this machine:" >&2
            cat "$rig_lock" >&2 2>/dev/null || true
            echo >&2
            echo "Two rounds on the same sticks make both meaningless. Wait for it, or set" >&2
            echo "RIG_NO_LOCK=1 if you are deliberately running against different sticks." >&2
            exit 1
        fi
        # Truncate now that the lock is held, then write who holds it.
        : > "$rig_lock"
        { echo "pid $$"; echo "out $out"; echo "started $(date -Iseconds)"
          echo "sticks ${RIG_STICK_A:-default} ${RIG_STICK_B:-default}"; } >> "$rig_lock"
    fi
fi
build="${SEABASS_BUILD_DIR:-$root/build}"
if [ "$rig_os" = "Darwin" ]; then
    A="${RIG_STICK_A:-/Volumes/VSTICKA}"
    B="${RIG_STICK_B:-/Volumes/VSTICKB}"
    deviceB="${RIG_DEVICE_B:-$(stick_device "$B")}"
else
    A="${RIG_STICK_A:-/media/sebas/RV2}"
    B="${RIG_STICK_B:-/media/sebas/A4-128GB}"
    deviceB="${RIG_DEVICE_B:-/dev/sdc1}"
fi
refA="${RIG_REFERENCE_A:-$HOME/Seabass/e2e/backups/CORSAIR.zip}"
refB="${RIG_REFERENCE_B:-$HOME/Seabass/e2e/backups/WHALESHARK2.zip}"
prints="${RIG_REFERENCE_PRINTS:-$HOME/Seabass/e2e/reference-fingerprints.txt}"
# rig_delete_backup refuses to delete anything at these paths or beside them.
export RIG_REFERENCE_A="$refA" RIG_REFERENCE_B="$refB"
export QT_QPA_PLATFORM=offscreen
# On Windows a GUI-subsystem process with no console attached has Qt's
# default message handler route qDebug()/qWarning()/console.log() (and
# QtQuickTest's own PASS/FAIL report) to OutputDebugString instead of
# stderr -- invisible to a pipe, a log file, or anything else short of a
# debugger. Every "this test produced no output at all" mystery tonight
# was this: the tests were genuinely failing, on a perfectly ordinary
# assertion, and there was simply nothing to read. Confirmed directly by
# setting this and immediately seeing the real failure message.
export QT_FORCE_STDERR_LOGGING=1
if rig_is_windows; then
    # Without this the offscreen platform's font backend on Windows has
    # no font directory to read and QFontDatabase::families() comes back
    # EMPTY -- confirmed directly, an assert inside Qt itself
    # ("!isEmpty()", qlist.h:652) the moment anything asks for a raw
    # font, which is exactly what F4's LiveFullStick test does before it
    # can say anything about the fill it exists to check. CMakeLists.txt
    # already sets this for seabass_qml_tests and interface_font_test;
    # the live QML binary this script calls directly needs it too, for
    # the same reason.
    export QT_QPA_FONTDIR="${SYSTEMROOT:-C:/Windows}/Fonts"
fi

mkdir -p "$out" "$out/shots"
# F4 writes gigabytes to stick A. An interrupt between the fill and its
# removal would leave the stick full for every later check and for the
# next round; this costs nothing when there is no filler.
trap 'rm -f "$A"/RIG-FILLER-*.bin "$B"/RIG-FILLER-*.bin 2>/dev/null; rm -rf "${rig_lock_dir:-}" 2>/dev/null' EXIT
# For S3: what the everyday profile looks like before the run.
# The everyday profile, listed so that a file APPEARING counts as a change
# too: the run creating ~/.config/seabass/seabass.conf is the damage this
# looks for, and "not there" has to be recorded as plainly as a size.
# Defined here, before its first use: bash resolves a function when the
# call runs, so a definition further down would have left the baseline
# empty and failed S3 and X4 on every round.
real_profile_now() {
    # macOS keeps the settings in a property list instead.
    for path in "$HOME/.config/seabass/seabass.conf" "$HOME/Library/Preferences/com.seabass.seabass.plist" \
                "$HOME/Seabass/metadata"; do
        if [ -e "$path" ]; then
            stat -c '%n %s %Y' "$path"
        else
            echo "$path ABSENT"
        fi
    done
    # And Windows in the registry, which no file listing can see -- see
    # everyday_settings_listing() in rig-platform.sh for why that made
    # this whole comparison vacuous there.
    everyday_settings_listing
}

real_profile_now > "$out/real-profile-before.txt"
summary="$out/summary.tsv"
: > "$summary"
a="$(basename "$A")"
b="$(basename "$B")"

# check NAME COMMAND... -- runs it into NAME.log; PASS when it exits 0 AND
# nothing inside it skipped. A skipped test proves nothing, and QtTest
# exits 0 for one, so a check that skips would otherwise read as green --
# the one thing this rig must never do before a release.
# RIG_ONLY="F4-stick-fills-up W8-delete-orphans" re-runs just those checks,
# after a fix say; the rest are neither run nor recorded, so the summary
# lists only what ran. Two things are never filtered: the restores that put
# a stick back at its reference (a partial run that skipped them left B off
# its reference, and the baseline would then be taken from a dirty stick),
# and the end of the run, which fails when a name matched nothing -- a typo
# must not read as a green run of zero checks.
rig_only_names() { printf '%s' "${RIG_ONLY:-}" | tr ',\t\n' '   '; }
rig_wants() {  # <check name> -> 0 when this run includes it
    [ -z "${RIG_ONLY:-}" ] && return 0
    # The restores from a REFERENCE only: FB8 also restores, from an
    # archive FB1 makes, and ran unasked against a missing one.
    [[ "$1" == B?-restore-? || "$1" == *-from-reference || "$1" == *-at-reference* ]] && return 0
    [[ " $(rig_only_names) " == *" $1 "* ]]
}
if [ -n "${RIG_ONLY:-}" ] && [ -z "$(rig_only_names | tr -d ' ')" ]; then
    echo "RIG_ONLY is set but names no check"
    echo "RIG RESULT: FAIL"
    exit 1
fi
ran=""

# The checks that run more than one test. Each writes a result per test
# into $RIG_PARTS (tools/rig-parts.sh, tools/rig_parts.hpp) and those
# lines go into the summary in place of its own single verdict, so the
# board can say which test failed instead of reddening all of them.
RIG_BUNDLES="R1-R3-read-A R1-R3-read-B R2-R5-pages W1-W3-W4-F1-F2-F3-live W2-W5-W6-edits FB1-FB2-backup-A C1-C5-backup-usb-stick E1-create-engine-library"

check() {
    local name="$1"; shift
    rig_wants "$name" || return 0
    ran="$ran $name"
    echo "$(date +%T) === $name"
    local parts=""
    case " $RIG_BUNDLES " in
        *" $name "*) parts="$out/$name.parts"; rm -f "$parts"; export RIG_PARTS="$parts" ;;
    esac
    local rc=0
    "$@" > "$out/$name.log" 2>&1 || rc=1
    unset RIG_PARTS
    if [ $rc -eq 0 ] && grep -qE "^SKIP|^ *SKIPPED" "$out/$name.log"; then
        echo "$(date +%T) --- $name: FAIL (it skipped; see $out/$name.log)"
        grep -E "^SKIP|^ *SKIPPED" "$out/$name.log" | head -5
        rc=1
    elif [ $rc -eq 0 ]; then
        echo "$(date +%T) --- $name: PASS"
    else
        echo "$(date +%T) --- $name: FAIL (see $out/$name.log)"
    fi
    if [ -z "$parts" ]; then
        printf '%s\t%s\n' "$name" "$([ $rc -eq 0 ] && echo PASS || echo FAIL)" >> "$summary"
    elif [ -s "$parts" ]; then
        cat "$parts" >> "$summary"
        echo "           $(grep -c . "$parts") test results recorded"
    else
        # A bundle that reported nothing has told the board nothing, and
        # the board would then keep whatever it said last time about
        # every test in it. Loud and red, under its own name, which the
        # recorder does not know and will say so about.
        echo "$(date +%T) --- $name: wrote no test results at all"
        printf '%s\tFAIL\n' "$name" >> "$summary"
        rc=1
    fi
    return $rc
}

catalogs() {  # stick -> sha256sum lines of its catalog files
    find "$1/PIONEER/rekordbox" "$1/Engine Library/Database2" -maxdepth 1 -type f \
        \( -name '*.pdb' -o -name '*.db' -o -name '*.db-wal' \) 2>/dev/null | sort | xargs -r -d '\n' sha256sum
}

unchanged_catalogs() {  # stick -- against the baseline taken after the restores
    # A write-ahead log is part of the baseline but not part of the
    # library: SQLite checkpoints it into the database and removes it,
    # which a save followed by an undo really does. A missing -wal whose
    # database still matches is therefore not a change to the catalogs,
    # and failing on it would have the rig crying wolf on every round.
    # Anything else -- a changed byte, a missing database -- still fails.
    local checked=0
    local bad=0
    while read -r sum file; do
        # sha256sum's own -c mode understands its "*" binary-mode marker,
        # but this hand-rolled parse doesn't -- and GNU coreutils' default
        # differs by platform: no marker on Linux, "*"-prefixed on this
        # MSYS2/Windows build (confirmed directly: `sha256sum somefile`
        # here prints "<hash> *somefile"). Left in, every comparison below
        # tests a path that can never exist, so every catalog file reads
        # MISSING regardless of its real state -- confirmed by reproducing
        # `catalogs()`'s own find/sha256sum by hand and getting real,
        # existing files, right after this check reported them all gone.
        file="${file#\*}"
        if [ ! -e "$file" ]; then
            case "$file" in
                *-wal|*-journal|*-shm)
                    local main="${file%-*}"
                    # The baseline is sha256sum's own output, so the
                    # path in it carries the same "*" binary marker this
                    # loop strips above -- on MSYS2/Windows, not on
                    # Linux. Matching " $main" against a line reading
                    # "<hash> *<path>" therefore never matched there, and
                    # a -wal that had simply been checkpointed away was
                    # reported MISSING: three checks in the Windows round
                    # 5 failed on one checkpoint. Compare the paths, with
                    # the hash and the marker taken off both sides.
                    if grep -F "$1/" "$out/catalog-baseline.txt" \
                        | sed 's/^[0-9a-f]*[[:space:]]*[*]\{0,1\}//' \
                        | grep -qxF "$main" && [ -f "$main" ]; then
                        echo "$file: gone (checkpointed into $(basename "$main"))"
                        continue
                    fi
                    ;;
            esac
            echo "$file: MISSING"
            bad=1
            continue
        fi
        checked=$((checked + 1))
        if [ "$(sha256sum "$file" | cut -d" " -f1)" = "$sum" ]; then
            echo "$file: OK"
        elif [ ! -e "$file-wal" ] && grep -F "$1/" "$out/catalog-baseline.txt" \
                | sed 's/^[0-9a-f]*[[:space:]]*[*]\{0,1\}//' | grep -qxF "$file-wal"; then
            # A database whose write-ahead log was in the baseline and is
            # now gone has been checkpointed: SQLite folded the log into
            # the database, which rewrites it. These bytes are SUPPOSED to
            # differ.
            #
            # Deliberately still a failure. This comparison is sha256 over
            # bytes and cannot tell "the log was folded in" from "the
            # library changed" -- both look exactly like this. Telling
            # them apart means reading the file as a database, and
            # exportLibrary.db is SQLCipher-encrypted, so neither
            # sha256sum nor sqlite3 can; only Seabass can. Until it does,
            # the round says what it knows rather than choosing.
            #
            # Windows round 7's F4-undo failed here, on the first run in
            # which an undo on a near-full stick actually completed --
            # which is also the first run since the nested-lock fixes in
            # which the save's own clean-up did anything at all.
            #
            # The test is narrow on purpose: the log was in the baseline
            # and is not on the stick now. A -wal that is still there has
            # not been folded into anything, so a database differing
            # beside one is not this case and keeps the bare message.
            echo "$file: DIFFERS, and its write-ahead log was checkpointed into it."
            echo "    A checkpoint rewrites the database, so differing bytes are expected here;"
            echo "    this check compares bytes and cannot tell that from a real change."
            echo "    Read the undo's own lines in the stick log above before calling it data loss."
            bad=1
        else
            echo "$file: DIFFERS"
            bad=1
        fi
    done < <(grep -F "$1/" "$out/catalog-baseline.txt")
    echo "$checked catalog file(s) compared"
    if [ "$checked" -eq 0 ]; then
        # The old one-liner failed here by accident (sha256sum -c refuses an
        # empty list); this one has to say so on purpose, or a baseline that
        # matches nothing turns every catalog check into a no-op.
        echo "NO catalog files matched the baseline for $1 -- nothing was compared"
        bad=1
    fi
    return $bad
}

references_unchanged() {
    local ok=0
    for ref in "$refA" "$refB"; do
        local name; name="$(basename "$ref")"
        local line; line="$(grep "^$name " "$prints")" || { echo "no fingerprint recorded for $name"; ok=1; continue; }
        local now="$name size=$(stat -Lc %s "$ref") mtime=$(stat -Lc %Y "$ref") manifest_sha256=$(unzip -p "$ref" SEABASS-MANIFEST.tsv | sha256sum | cut -d' ' -f1)"
        echo "recorded: $line"; echo "now:      $now"
        [ "$line" = "$now" ] || ok=1
        [ ! -w "$(readlink -f "$ref")" ] || { echo "$name is writable"; ok=1; }
    done
    return $ok
}

# corpus_test runs on its own, after the rest. It is the longest test
# there is -- every matrix case copies and rescans a whole library -- and
# under -j6 it competes with five siblings for one disk. On Windows that
# tipped it past the 900 s cap while the same test, alone on the same
# machine, finishes in 511 s; on Linux it takes 27 s either way. So the
# cap stayed honest and the scheduling was what failed. Split out, each
# side of the line means one thing: S1-suite is "everything else
# passed", S1-corpus is "the long one passed", and neither can be red
# because of the other.
suite() {
    # A stick, so the two screenshot cases that want one stop skipping.
    # They are the only tests in the non-live suite that read
    # SEABASS_LIVE_STICK, and they only read it: they point the Metadata
    # Backup and Restore pages at the stick's catalogs and grab the
    # result. Everything else in the suite ignores it.
    #
    # B rather than A because B is the stick this rig treats as
    # expendable, and this runs before B1/B3 have put either back to its
    # reference, so whatever state the last round left is what gets
    # photographed. That is fine for a screenshot and would not be for
    # an assertion.
    #
    # ctest alone leaves them skipped, which is how they have always
    # run: a skip in a release gate proves nothing, and these two are
    # the only pages whose screenshots show real stick data.
    SEABASS_LIVE_STICK="$B" ctest --test-dir "$build" -j6 --timeout 900 --output-on-failure -E '^corpus_test$'
}

corpus() {
    ctest --test-dir "$build" --timeout 1800 --output-on-failure -R '^corpus_test$'
}

# S2: the folder the app is pointed at holds links to the references, and
# the originals themselves cannot be written.
reference_links() {
    local ok=0
    for ref in "$refA" "$refB"; do
        local link="$(dirname "$refA")/$(basename "$ref")"
        [ -L "$link" ] || { echo "$link is not a link"; ok=1; }
        local target; target="$(readlink -f "$link" 2>/dev/null)"
        [ -f "$target" ] || { echo "$link points nowhere"; ok=1; }
        [ ! -w "$target" ] || { echo "$target is writable"; ok=1; }
        echo "$link -> $target ($(stat -Lc %A "$target"), $(stat -Lc %s "$target") bytes)"
    done
    return $ok
}

# S3: settings, metadata store and edit locks come from the throwaway
# profile. Proven by what the run wrote there, and by the everyday
# profile being untouched: the runner records it before and after.
sandbox_profile() {
    local ok=0
    # Unguarded expansions would abort the whole run under `set -u` -- and
    # XDG_DATA_HOME in particular is routinely unset.
    local home="${SEABASS_HOME:-}"
    local config="${XDG_CONFIG_HOME:-}"
    local data="${XDG_DATA_HOME:-}"
    echo "SEABASS_HOME=$home"
    echo "XDG_CONFIG_HOME=$config"
    echo "XDG_DATA_HOME=$data"
    case "$home" in "$HOME/Seabass/e2e"*) ;; *) echo "SEABASS_HOME is not the sandbox"; ok=1;; esac
    case "$config" in "$HOME/Seabass/e2e"*) ;; *) echo "XDG_CONFIG_HOME is not the sandbox"; ok=1;; esac
    case "$data" in "$HOME/Seabass/e2e"*) ;; *) echo "XDG_DATA_HOME is not the sandbox"; ok=1;; esac
    # XDG_CONFIG_HOME means nothing to Qt on macOS; there the checks run
    # the QML runner, which keeps its settings in a sandbox of its own, and
    # the everyday property list is watched below instead.
    #
    # Nor on Windows, where a real run resolves to the registry whatever
    # this variable says (gui/seabass_settings.hpp records the
    # measurement). Asserting a file there fails a check that can never
    # pass, and says nothing about the thing that matters -- so the
    # everyday key is watched below instead, exactly as on macOS. That
    # only works while the tool to read it is there, so a missing reg.exe
    # fails here rather than silently comparing two placeholders.
    if [ "$rig_os" != "Darwin" ] && ! rig_is_windows; then
        [ -f "$config/seabass/seabass.conf" ] || { echo "no settings in the sandbox"; ok=1; }
    fi
    if ! everyday_settings_watchable; then
        echo "reg.exe not on PATH: the everyday settings cannot be watched, so this round cannot prove it left them alone"
        ok=1
    fi
    [ -d "$home/metadata" ] || { echo "no metadata store in the sandbox"; ok=1; }
    real_profile_now > "$out/real-profile-after.txt"
    diff "$out/real-profile-before.txt" "$out/real-profile-after.txt" \
        && echo "the everyday profile is exactly as it was" \
        || { echo "THE EVERYDAY PROFILE CHANGED"; ok=1; }
    return $ok
}


# Run again at the very end of the round: S3 alone only proves the profile
# was untouched by the three checks before it.
sandbox_profile_still_clean() {
    # The same refusal S3 makes, and for the same reason: without reg.exe
    # the Windows part of the listing is a placeholder, identical before
    # and after, so this diff would agree having compared nothing. S3
    # failing does not cover it -- RIG_ONLY runs a check by name, and then
    # X4 is the only guard there is.
    if ! everyday_settings_watchable; then
        echo "reg.exe not on PATH: the everyday settings cannot be watched, so this round cannot prove it left them alone"
        return 1
    fi
    real_profile_now > "$out/real-profile-end.txt"
    diff "$out/real-profile-before.txt" "$out/real-profile-end.txt" \
        && echo "the everyday profile is still exactly as it was" \
        || { echo "THE EVERYDAY PROFILE CHANGED DURING THE ROUND"; return 1; }
}

# W8: deletes audio for good, so the stick is restored right after.
delete_orphans() {
    SEABASS_RIG_DELETE_ORPHANS=1 live_test "$B" LiveEditMode::test_13_pendingDeletionsCancelThenComplete
}

# One test function per process, by its FULL name: a bare TestCase name
# makes the QtQuickTest runner exit 0 without running a thing, which is a
# check that silently passes -- the worst kind this rig can have.
# live_test for exactly one test, with its board row. Used where a check
# runs several and each is answerable for itself.
live_one_test() {  # <board id> <stick> <full test name>
    local part="$1"; shift
    local rc=0
    live_test "$@" || rc=1
    rig_part_rc "$part" "$rc"
    return $rc
}

live_test() {  # <stick> <full test name>...
    local failed=0
    local stick="$1"; shift
    # Without this every live test skips itself ("SEABASS_LIVE_STICK is not
    # set") and, before check() learnt to fail on a skip, wrote PASS having
    # run nothing. run-live.sh and rig-edits.sh export it themselves; these
    # checks call the binary directly and must too.
    # B by default, but a caller that names a stick keeps it: F4 fills A
    # and must run against A, and an unconditional export here beat its
    # prefix -- so F4 tested the stick that was never filled.
    export SEABASS_LIVE_STICK="$stick"
    # local, or this loop overwrites check()'s own $name through bash's
    # dynamic scoping, and the summary then lists a bundle under its last
    # sub-test ("LivePages::test_03" for R2-R5) -- round 4 read that way.
    local name
    # The stick keeps its own log of what every save did, and until now
    # the rounds threw it away: the round output said PASS or FAIL with
    # no reasoning attached, and the one file that could have said WHY
    # sat on the stick until the next check overwrote the catalogs.
    # macOS round 7 is the case in point -- F4 was green there for weeks
    # because the save was refused before it ever took a backup, and
    # nobody could tell from the round which of the two refusals it was.
    # Collected per test, from the line the test started at, so each
    # check's log carries only its own.
    local stick_log="$stick/Seabass/seabass.log"
    for name in "$@"; do
        echo "=== $name"
        local log="$out/live-${name//:/_}.txt"
        local before=0
        [ -f "$stick_log" ] && before=$(wc -l < "$stick_log")
        "$build/seabass_qml_tests" -input "$root/tests/qml-live" "$name" 2>&1 | tee "$log"
        [ "${PIPESTATUS[0]}" -eq 0 ] || failed=1
        if grep -q "^SKIP" "$log"; then
            echo "   SKIPPED, which counts as a failure here"
            failed=1
        fi
        if [ -f "$stick_log" ] && [ "$(wc -l < "$stick_log")" -gt "$before" ]; then
            echo "--- $stick_log, what this test added:"
            tail -n +$((before + 1)) "$stick_log" | sed 's/^/    /'
        elif [ ! -f "$stick_log" ]; then
            # Not a failure on its own: a read-only check writes nothing.
            # Said out loud so "no log" is never mistaken for "no lines".
            echo "--- no $stick_log (this test wrote nothing to the stick)"
        fi
    done
    return $failed
}

# R2 and R5: read-only pages. R5 is pointed at the folder of links.
live_pages() {
    export SEABASS_RIG_REFERENCE_DIR="$(dirname "$refA")"
    local rc=0
    live_one_test R2-statistics "$B" LivePages::test_01_statisticsLoads || rc=1
    live_one_test R2-performance "$B" LivePages::test_02_performanceLoads || rc=1
    live_one_test R5-manage-backups "$B" LivePages::test_03_manageBackupsListsTheReferences || rc=1
    # Always, not only when the three passed: three read-only pages that
    # leave the stick changed is a different fault from any of them
    # failing, and it has its own row now.
    if unchanged_catalogs "$B"; then rig_part pages-wrote-nothing PASS; else rig_part pages-wrote-nothing FAIL; rc=1; fi
    return $rc
}

# E1: Create Engine Library, the one feature a player has already called
# corrupt on us (a Prime 4 rejected the first library it was ever given,
# RV2, 2026-09-17) and the one with no rig row until now.
#
# make_engine_library reads the stick's rekordbox export the way the app
# does, runs the creator into a folder, and then reads the result back
# with this project's own Engine reader, so a library that cannot be read
# fails here instead of on the player. Generation 3 is what current Denon
# hardware writes for itself; the tool takes 1 and 2 for the firmware
# matrix nobody has verified yet, and a round that wants one passes
# RIG_ENGINE_GENERATION.
#
# Into $out, never onto the stick: creating a candidate library must not
# touch the library it was read from, and the second row says whether it
# did. That is a different fault from the creation failing and it gets its
# own answer, the same way live_pages reports pages-wrote-nothing.
create_engine_library() {
    local dir="$out/engine-library"
    # A previous round's candidate would make the creator refuse, and a
    # refusal is not the answer this check is asking for.
    rm -rf "$dir"
    local rc=0
    "$build/make_engine_library" "$B" "$dir" "${RIG_ENGINE_GENERATION:-3}" || rc=1
    rig_part E1-engine-library-created "$([ $rc -eq 0 ] && echo PASS || echo FAIL)"
    if unchanged_catalogs "$B"; then
        rig_part engine-creation-wrote-nothing PASS
    else
        rig_part engine-creation-wrote-nothing FAIL
        rc=1
    fi
    return $rc
}

# F5: leaving with unsaved changes, both ways out.
quit_with_changes() {
    live_test "$B" LiveQuit::test_01_discardLeavesTheStickAlone LiveQuit::test_02_saveThenLeaveWritesEverything \
        && unchanged_catalogs "$B"
}

# F4: fill the stick to within a few MB, try to save, then put the space
# back whatever happened -- the filler is removed even on a failure.
free_kb_of() { /bin/df -kP "$1" | awk 'NR==2 {print $4}'; }

# The filler is a set of pieces, not one file: FAT32 holds nothing over
# 4 GiB, and a single filler stopped there with "File too large" -- the
# stick kept 13 GB free and F4 proved nothing (macOS round 2). exFAT would
# take one file, but one path for both beats two.
filler_pieces() { ls -1 "$A"/RIG-FILLER-*.bin 2>/dev/null | sort; }
filler_newest() { filler_pieces | tail -1; }

# Writes <KB>, in pieces of at most 3 GiB, continuing the numbering.
filler_grow() {  # <KB>
    local todo_kb="$1"
    local chunk_kb=3145728  # 3 GiB
    local piece; piece=$(filler_pieces | wc -l | tr -d ' ')
    while [ "$todo_kb" -gt 0 ]; do
        local now_kb=$todo_kb
        [ "$now_kb" -gt "$chunk_kb" ] && now_kb=$chunk_kb
        dd if=/dev/zero of="$(printf '%s/RIG-FILLER-%02d.bin' "$A" "$piece")" \
            bs=1M count=$((now_kb / 1024)) status=none || true
        todo_kb=$((todo_kb - now_kb))
        piece=$((piece + 1))
    done
}

# Gives <KB> back, from the newest piece down: the same "shrink rather
# than rewrite" the single filler allowed, since rewriting 13 GB costs
# eleven minutes over USB 2 for less than a megabyte of difference.
filler_shrink() {  # <KB>
    local todo_kb="$1"
    while [ "$todo_kb" -gt 0 ]; do
        local last; last=$(filler_newest)
        [ -n "$last" ] || return 0
        local piece_kb=$(( $(stat -c %s "$last") / 1024 ))
        if [ "$piece_kb" -gt "$todo_kb" ]; then
            truncate -s -${todo_kb}K "$last" || true
            return 0
        fi
        rm -f "$last"
        todo_kb=$((todo_kb - piece_kb))
    done
}

fill_and_run() {  # <leave KB> <full test name> <records may appear: 0|1> <keep the filler for the next pass: 0|1>
    local leave_kb="$1" test="$2" records_may_appear="$3" keep_filler="$4"
    # Stick A, deliberately: exFAT has no fallocate, so the filler is
    # written for real, and A's spare gigabytes cost minutes where B's cost
    # the better part of an hour for exactly the same proof.
    # The newest piece, for the "records left behind" check below.
    local filler; filler=$(filler_newest)
    local free_kb; free_kb=$(free_kb_of "$A")
    # Under a quarter megabyte, and measured rather than assumed: the
    # backup this save writes is about 460 KB (the analysis file plus
    # exportLibrary.db), so a megabyte of slack let the save succeed and
    # F4 proved the opposite of its name. Below the backup's own size the
    # save has to refuse, which is the outcome this check exists for.
    local size_kb=$((free_kb - leave_kb))
    local rc=0
    if [ -n "$filler" ] && [ "$free_kb" -lt "$leave_kb" ]; then
        # The filler from the pass before, shrunk to the new margin: exFAT
        # shrinks a file in place, where writing 13 GB again costs eleven
        # minutes over USB 2 for less than a megabyte of difference. (Seen
        # before the "too little to fill" guard below, which the kept
        # filler would trip: 256 KB free IS too little to fill from.)
        echo "shrinking the filler from the pass before: $free_kb KB free -> leaving about $leave_kb KB"
        filler_shrink $((leave_kb - free_kb))
        sync
    elif [ "$size_kb" -lt "$leave_kb" ]; then
        # What the stick actually has, not the margin: this is the one
        # path that gives up on F4, so it should not misdescribe why. A
        # filler left from a pass before goes too, or the restore that
        # follows finds a full stick.
        echo "$A has $free_kb KB free, too little to fill down to $leave_kb KB; F4 cannot be proven here"
        rm -f "$A"/RIG-FILLER-*.bin
        sync
        return 1
    else
        echo "filling $A: $free_kb KB free -> leaving about $leave_kb KB"
        # Growing: a filler kept from a pass whose margin was larger gains
        # pieces rather than being cut to nothing and written again.
        filler_grow "$size_kb"
        sync
    fi
    # bs=1M rounds the filler down by up to a megabyte, and the old limit
    # of four times the margin let that through: round 4 was left with
    # 928 KB, room enough for the save's 460 KB backup, so the save fitted
    # and F4 passed having tested a stick with space on it. Top up in
    # cluster-sized steps (exFAT on these sticks: 32 KiB) to the margin,
    # then measure again; the limit is one cluster of rounding, no more.
    local left_kb; left_kb=$(free_kb_of "$A")
    if [ "$left_kb" -gt "$leave_kb" ]; then
        # Its own small piece: appending to the newest one could take it
        # over the 4 GiB a FAT32 file may hold.
        dd if=/dev/zero of="$A/RIG-FILLER-top.bin" bs=32K count=$(((left_kb - leave_kb) / 32)) status=none || true
        sync
        left_kb=$(free_kb_of "$A")
    fi
    filler=$(filler_newest)
    /bin/df -hP "$A" | tail -1
    echo "left on $A after the fill: $left_kb KB (target $leave_kb KB)"
    if [ "$left_kb" -gt $((leave_kb + 32)) ]; then
        # The fill did not take, so a save that fits proves nothing about a
        # full stick. Said out loud rather than passed over: dd's own error
        # is swallowed so the filler is always removed.
        echo "the stick still has $left_kb KB free: the fill failed, F4 cannot be proven"
        rm -f "$A"/RIG-FILLER-*.bin
        sync
        return 1
    fi
    # A save refused for lack of space must leave no backup record behind:
    # the one it started, a truncated backup.zip with no manifest, used
    # to stay, and Manage Backups listed it as an empty entry. Records are
    # directories; the save's .write.lock beside them is a file and stays.
    local records_before; records_before=$(find "$A/Seabass/backups" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
    SEABASS_RIG_FULL_STICK=1 live_test "$A" "$test" || rc=1
    # WHICH refusal this was, because only one of the two can see the bug
    # this check is named after.
    #
    #  (a) the backup itself did not fit. runSaveLoop returns the moment
    #      backupAllNow() throws and never reaches
    #      discardBackupsTakenThisSave(); the store clears up its own
    #      half-written record, so nothing is left and the count below
    #      passes -- having tested nothing about the discard.
    #  (b) the backup fitted and the change then failed to write. This is
    #      the only path that calls discardBackupsTakenThisSave(), and the
    #      only one in which a leftover record means what the check says.
    #
    # Which one a machine lands on is decided by free space, not by
    # platform: the fill is the same on all three, but how much of it a
    # backup needs depends on cluster size and the catalog. macOS took
    # (a) for round after round and reported a green F4 the whole time,
    # which reads as "the discard path works" and meant no such thing.
    local refusal; refusal=$(cat "$out"/live-*FullStick*.txt 2>/dev/null)
    if printf '%s' "$refusal" | grep -q "could not back up before saving"; then
        echo "NOTE: the save was refused because the BACKUP did not fit, so it returned before"
        echo "      discardBackupsTakenThisSave() could run. The record count below is still"
        echo "      worth having, but this pass says nothing about the discard path -- leave a"
        echo "      little more room (fill_and_run's margin) for a round that exercises it."
    elif printf '%s' "$refusal" | grep -qE "failed to durably write|[Nn]o space left"; then
        echo "NOTE: the backup fitted and the change then failed, so the discard path DID run:"
        echo "      the record count below is a real test of it."
    fi
    # Counted twice, with a sync between: the first count runs the moment
    # the test process exits and has caught a record that was gone a
    # second later, three rounds running. A directory entry a save has
    # just removed can still be listed on these sticks, and "the rig saw
    # it for a moment" is not the same claim as "the refused save left it
    # behind" -- only the second is a bug, and the round should say which
    # it found rather than make the reader guess.
    sync
    local records_after; records_after=$(find "$A/Seabass/backups" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
    local records_settled; records_settled=$records_after
    if [ "$records_after" -ne "$records_before" ]; then
        sleep 2
        sync
        records_settled=$(find "$A/Seabass/backups" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
    fi
    # A save that FITTED keeps its record, and should: that record IS the
    # undo the user has just been offered. Saying "the refused save left a
    # backup record behind" about a save that was not refused is a second,
    # wrong answer next to the real failure -- the fill left too much room,
    # which the test itself reports. Round 8's first run of
    # F4-save-fails-after-its-backup printed both and the record line was
    # the louder one.
    local save_fitted=0
    grep -q "the save FITTED" "$out"/live-*FullStick*.txt 2>/dev/null && save_fitted=1
    if [ "$records_may_appear" -eq 0 ] && [ "$save_fitted" -eq 0 ] \
       && [ "$records_settled" -ne "$records_before" ]; then
        echo "the refused save left a backup record behind: $records_before -> $records_settled entries in $A/Seabass/backups"
        # The record this save made, not the folder's oldest eight: a
        # stick that already holds backups would have filled the listing
        # with those and hidden the one the message is about. What is in
        # it decides how bad it is -- a record with a manifest is a
        # backup somebody can restore, one without is the truncated
        # half-record Manage Backups used to list as an empty entry.
        while read -r record; do
            [ -n "$record" ] || continue
            echo "  $record"
            find "$record" -maxdepth 1 -mindepth 1 2>/dev/null | sed 's/^/    /'
        done < <(find "$A/Seabass/backups" -mindepth 1 -maxdepth 1 -type d -newer "$filler" 2>/dev/null)
        rc=1
    elif [ "$records_after" -ne "$records_before" ] && [ "$records_settled" -eq "$records_before" ]; then
        # Only when it really did go: F4-undo's own record is allowed to
        # appear AND to stay, and saying it had gone would be a third
        # wrong answer about the same folder.
        echo "a record was listed the moment the save ended and was gone a second later ($records_after -> $records_settled):" \
             "the removal lands after the process exits, so this is the rig looking too early, not a leftover"
    fi
    if [ "$keep_filler" -eq 1 ]; then
        echo "filler kept for the next pass; $(/bin/df -hP "$A" | awk 'NR==2 {print $4}') free"
    else
        rm -f "$A"/RIG-FILLER-*.bin
        sync
        echo "filler removed; $(/bin/df -hP "$A" | awk 'NR==2 {print $4}') free again"
    fi
    unchanged_catalogs "$A" || rc=1
    return $rc
}

# F4: fill to under a quarter megabyte and the save has to refuse -- the
# backup it writes first is about 460 KB (measured; the margin is below
# that on purpose, see fill_and_run).
full_stick() {
    local keep=0
    rig_wants F4-undo-on-a-nearly-full-stick && keep=1
    fill_and_run 256 LiveFullStick::test_saveOnAFullStickFailsCleanly 0 "$keep"
}

# Issue #27: the same stick filled to leave room for the save and not
# for the undo. The save needs about 1.3 MB: its backup record (460 KB),
# the checkpoint copies of the two files it writes (520 KB) and one
# temporary (330 KB); 1.2 MB fitted it once and refused it a cluster
# later, so 1.6 MB. The undo then finds about 1.1 MB and needs about
# 1.25 MB (the copy of what it overwrites, one temporary, the margin),
# so it is refused for space up front, saying so -- a refusal that
# blamed the backup is what #27 was. Records may appear: the save's own,
# and the undo's pre-restore copy if it ever gets that far.
# F4, the margin in between, and the only one of the three that can see
# a backup record left behind.
#
# A save on a full stick is refused in one of two places, and they are
# not interchangeable:
#
#   256 KB  the BACKUP does not fit. runSaveLoop returns the moment
#           backupAllNow() throws, before discardBackupsTakenThisSave()
#           can run at all; the store clears up its own half-written
#           record, nothing is left, and the check passes having tested
#           nothing about the discard.
#   1600 KB everything fits. The save succeeds; this is the undo's pass.
#
# Between them is a window where the backup fits (about 460 KB on these
# sticks) and the writes that follow it do not -- the only refusal that
# reaches the discard path, and so the only one that can catch the
# record this check is named after. Both machines that ran F4 landed on
# 256 KB for round after round and reported green: macOS for weeks, and
# Linux on the very round that was meant to prove the fix for
# seabass#F4. Neither was wrong about what it measured; the check simply
# could not fail.
#
# The number is measured on this rig, and it is narrow. Round 8 walked it:
# at 256 KB the backup does not fit (path one), at 768 KB everything fits
# and the save succeeds, which the test reports as "the save FITTED" and
# the rig counts as a failure. So the window is between them, and 512 KB
# sits above the backup's ~460 KB and below what the ANLZ pair and the
# database need after it.
#
# It is a margin, not a constant, and it depends on the catalog and the
# filesystem's cluster size. That is survivable only because BOTH ways of
# missing it are loud: a save that fits says so and fails, and a save
# refused before its backup gets the NOTE above saying the discard path
# was never reached. Neither can pass quietly, which is the whole reason
# this check exists.
full_stick_after_its_backup() {
    fill_and_run 512 LiveFullStick::test_saveOnAFullStickFailsCleanly 0 0
}

full_stick_undo() {
    # Keep the filler when C6 is going to run: it is the check's only
    # chance at a target too small for the source, and refilling 28 GiB
    # over USB 2 afterwards costs twenty minutes for a state that is
    # already on the stick. C6 removes it.
    local keep=0
    rig_wants C6-target-too-small && keep=1
    fill_and_run 1600 LiveFullStick::test_undoOnANearlyFullStick 1 "$keep"
}

cli_sync_dry_run() {
    "$build/seabass-cli" sync --rekordbox "$B/PIONEER" --engine "$B/Engine Library" --dry-run && unchanged_catalogs "$B"
}

# One line per entry: every file with its size and mtime, every
# directory by name alone.
#
# Seabass/caches is left out: it holds derived data (probed durations,
# catalog mirrors) that any run may rebuild and that carries nothing of
# the library. Everything else is compared, including Seabass/backups,
# so a lock file left behind still fails this check.
#
# A directory's mtime is not compared, because it moves whenever
# anything inside it does -- including inside the caches this check
# deliberately ignores, which is how the Seabass folder itself came back
# as the only difference. The files under it are compared exactly, so
# nothing real hides behind that.
stick_tree() {  # stick -> one line per file: path, size, mtime; per directory: path
    find "$1" -mindepth 1 -path "$1/Seabass/caches" -prune -o \
        -printf '%y\t%P\t%s\t%T@\n' 2>/dev/null \
        | awk -F'\t' '$1 == "d" { print "d\t" $2; next } { print }' | sort
}

# A command that says it only reads must leave the stick exactly as it
# was -- every file, not only the catalogs unchanged_catalogs compares.
#
# `scan` took the stick's write lock before *offering* to consolidate
# duplicate cues, and taking that lock creates Seabass/backups/.write.lock
# and leaves it there. Every scan of every stick, for a write almost none
# of them made. The rig could not have caught it: unchanged_catalogs
# would not have looked at that file, and the rig never ran `scan` at
# all -- its one CLI call is R4's dry run, which returns before any lock.
# So this check exists to run the commands that claim to read, and to
# compare the whole tree rather than the files someone thought to name.
#
# Stdin is closed: a scan that does find duplicates asks before writing,
# and a rig check must never sit at a prompt.
read_only_writes_nothing() {
    local before after
    # What the caches held before, so this check can put them back. A
    # check that leaves the stick different from how it found it can hide
    # the next one's bug -- and a scan writes a duration cache, which is
    # allowed but is still state the following checks did not ask for.
    local cachesBefore; cachesBefore="$(find "$B/Seabass/caches" -mindepth 1 2>/dev/null | sort)"
    before="$(stick_tree "$B")"
    "$build/seabass-cli" scan --rekordbox "$B/PIONEER" < /dev/null || return 1
    "$build/seabass-cli" scan --engine "$B/Engine Library" < /dev/null || return 1
    after="$(stick_tree "$B")"
    # Anything under caches this check caused goes again, before the
    # verdict, so the removal happens whether it passes or fails.
    find "$B/Seabass/caches" -mindepth 1 2>/dev/null | sort | while read -r cached; do
        printf '%s\n' "$cachesBefore" | grep -qxF "$cached" || rm -rf "$cached"
    done
    if [ "$before" = "$after" ]; then
        return 0
    fi
    echo "a read-only command changed the stick:"
    diff <(printf '%s\n' "$before") <(printf '%s\n' "$after") | head -20
    return 1
}

live_edit_mode() {
    local rc=0
    "$root/tests/qml-live/run-live.sh" "$B" "$deviceB" "$out/shots" || rc=1
    # run-live.sh has written a row per test by now. This is the row for
    # the bundle's other claim: that all of it put the stick back exactly
    # as it found it. It runs whatever the tests did, because a stick
    # left changed after a failed test is worth knowing about.
    if unchanged_catalogs "$B"; then rig_part live-wrote-nothing PASS; else rig_part live-wrote-nothing FAIL; rc=1; fi
    return $rc
}

metadata_between_sticks() {
    # A difference to carry, planted first. The check takes stick B's
    # metadata onto stick A, and a round where one fixture was restored
    # to both sticks has nothing to take: round 5 saw 107 tracks seen, 5
    # proposals, 0 conflicts, on all three platforms, and the check
    # failed for want of a library difference rather than for anything
    # the code did. So B gets a cue of its own first, and is put back
    # afterwards whatever the check decides. keep_cue waits out FAT's
    # two-second mtime resolution, the same reason rig-clones.sh does.
    echo "=== planting a difference on $B for the metadata to carry"
    sleep 3
    if ! SEABASS_LIVE_STICK="$B" SEABASS_RIG_KEEP_CUE_MS=45000 \
        "$build/seabass_qml_tests" -input "$root/tests/qml-live" LiveEditMode::test_11_rigKeepCue; then
        echo "could not plant a cue on $B, so there is nothing for W7 to carry"
        return 1
    fi
    local rc=0
    SEABASS_LIVE_STICK="$A" SEABASS_LIVE_SECOND_STICK="$B" \
        "$build/seabass_qml_tests" -input "$root/tests/qml-live" LiveEditMode::test_12_metadataFromSecondStickSaveUndo \
        || rc=1
    unchanged_catalogs "$A" || rc=1
    # B carried the planted cue, so it is no longer at its reference:
    # put it back before the checks that assume it is.
    echo "=== putting $B back to its reference after the planted cue"
    "$build/rig_restore" "$refB" "$B" --execute || rc=1
    return $rc
}

refused_while_dj_software_runs() {
    local fake=""
    # Windows: the detector (rekordbox_process_detector.cpp) matches only
    # on the running process's exact image name ("rekordbox.exe"), so the
    # real, already-installed app satisfies it just by existing in the
    # process list -- no need to reach a usable UI state, and taskkill
    # cleans it up regardless of what it's doing when the check finishes.
    # Elsewhere there is no such install, so start_fake_dj puts the name
    # on a process the rig built for it.
    if rig_is_windows; then
        local rekordbox_exe="${RIG_REKORDBOX_EXE:-/c/Program Files/rekordbox/rekordbox 7.2.18/rekordbox.exe}"
        # The default path is version-pinned, so the next rekordbox update
        # moves it -- caught here with a clear message rather than as a
        # bash "no such file" buried inside this check's log.
        [ -x "$rekordbox_exe" ] || { echo "rekordbox.exe not found at $rekordbox_exe -- set RIG_REKORDBOX_EXE"; return 1; }
        "$rekordbox_exe" &
        sleep 3
    else
        start_fake_dj "$build" 120 || return 1
        fake=$fake_dj_pid
        sleep 1
    fi
    "$build/rig_backup" "$B" "$out/backups-fb/$b.zip" --expect-refused
    local rc=$?
    if rig_is_windows; then
        # rekordboxAgent.exe is a persistent watchdog that relaunches
        # rekordbox.exe if it's killed while the agent is still up --
        # confirmed directly: a single taskkill //IM rekordbox.exe left
        # it detected as running for 11+ seconds after, failing five
        # later checks that never got a real answer either way. Kill the
        # agent first so it stops respawning, then retry the main exe
        # until tasklist actually shows it gone rather than trusting one
        # taskkill call.
        taskkill //F //IM rekordboxAgent.exe >/dev/null 2>&1 || true
        for _ in 1 2 3 4 5; do
            taskkill //F //IM rekordbox.exe >/dev/null 2>&1 || true
            sleep 1
            tasklist //FI "IMAGENAME eq rekordbox.exe" 2>/dev/null | grep -qi rekordbox.exe || break
        done
    else
        kill "$fake" 2>/dev/null
        wait "$fake" 2>/dev/null
    fi
    return $rc
}

# FB1, with FB3's seed files put on A first so FB3 has something to
# change: part of the check body, so RIG_ONLY filters the seeding with
# the backup and a seed failure is FB1's, not FB3's.
backup_a_seeded() {
    if ! seed_fb3; then
        # Nothing seeded, so nothing is backed up and neither test ran.
        # Said out loud: unreported, the board keeps last round's green.
        rig_part FB1-backup-A FAIL
        rig_part FB2-archive-sound-A FAIL
        return 1
    fi
    env RIG_PART_SUFFIX=-A "$build/rig_backup" "$A" "$out/backups-fb/$a.zip"
}

seed_fb3() {
    mkdir -p "$A/RIG-FB3"
    head -c 1048576 /dev/urandom > "$A/RIG-FB3/change-me.bin"
    head -c 1048576 /dev/urandom > "$A/RIG-FB3/remove-me.bin"
    sync
}

change_fb3() {
    if [ ! -d "$A/RIG-FB3" ] || [ ! -f "$out/backups-fb/$a.zip" ]; then
        echo "FB3 changes FB1's seed files and increments FB1's archive: run FB1-FB2-backup-A with it"
        return 1
    fi
    sleep 2
    head -c 1048576 /dev/urandom > "$A/RIG-FB3/change-me.bin"
    rm -f "$A/RIG-FB3/remove-me.bin"
    head -c 524288 /dev/urandom > "$A/RIG-FB3/added.bin"
    sync
    "$build/rig_backup" "$A" "$out/backups-fb/$a.zip" --expect 1,1,1,1572864
}

resume_after_keep() {
    "$build/rig_backup" "$B" "$out/backups-fb/$b.zip" --cancel-at 20 keep && "$build/rig_backup" "$B" "$out/backups-fb/$b.zip"
}

# ---- setup and suite -------------------------------------------------
check S1-suite suite
check S2-reference-links reference_links
check S3-sandboxed-profile sandbox_profile
check S4-references-unchanged references_unchanged

# ---- the stick's own filesystem --------------------------------------
#
# On a FAT32 loopback image rig_fs_repair damages itself, not on a stick:
# the detection, the platform's repair call and the files surviving it are
# the same code either way, and a stick cannot be damaged on purpose
# without root or luck (see the ticket on damaging one from seabass-cli).
# Windows has no unprivileged way to attach an image, so the check is not
# built there and the repair stays a manual check on that platform.
if ! rig_is_windows && [ -x "$build/rig_fs_repair" ]; then
    check H1-filesystem-repair "$build/rig_fs_repair"
fi

# ---- restores onto the test sticks -----------------------------------
check B1-restore-A "$build/rig_restore" "$refA" "$A" --execute || { echo "stick A is not at its reference; stopping"; exit 1; }
check B3-restore-B "$build/rig_restore" "$refB" "$B" --execute || { echo "stick B is not at its reference; stopping"; exit 1; }
{ catalogs "$A"; catalogs "$B"; } > "$out/catalog-baseline.txt"

# ---- reads -----------------------------------------------------------
check R1-R3-read-A env RIG_PART_SUFFIX=-A "$build/rig_read" "$A" "$refA"
check R1-R3-read-B env RIG_PART_SUFFIX=-B "$build/rig_read" "$B" "$refB"
check R2-R5-pages live_pages
check R4-sync-dry-run cli_sync_dry_run
check R7-read-only-writes-nothing read_only_writes_nothing
check R6-catalogs-after-reads bash -c "grep -F '$A/' '$out/catalog-baseline.txt' | sha256sum -c && grep -F '$B/' '$out/catalog-baseline.txt' | sha256sum -c"
check E1-create-engine-library create_engine_library

# ---- edits -----------------------------------------------------------
check W1-W3-W4-F1-F2-F3-live live_edit_mode
check W2-W5-W6-edits "$root/tools/rig-edits.sh" "$B" "$out/catalog-baseline.txt"
# Directly after the saves, and before W7. A restore to the reference
# REPLACES the stick, Seabass/backups with it, so every automatic record
# the saves left is gone afterwards -- and metadata_between_sticks ends
# with exactly such a restore, because its planted cue takes B off its
# reference. W9 used to sit after that and count what was left: two
# records, both made by W7 itself in the seconds before its restore, and
# never the three it asks for. It failed every full round for the order
# it ran in rather than for anything a save did.
#
# Measured on macOS 2026-09-21, stick B at its reference: rig-edits.sh
# alone leaves six records -- add-cue, duplicate-file-cleanup and
# consistency-repair, each with the pre-restore copy its undo takes --
# and the reference restore leaves none.
check W9-saves-left-backups "$build/rig_save_backups" "$B" --expect-at-least 3
check W7-metadata-between-sticks metadata_between_sticks
check F5-quit-with-unsaved-changes quit_with_changes

# ---- full stick backups ----------------------------------------------
mkdir -p "$out/backups-fb"
check FB1-FB2-backup-A backup_a_seeded
check FB3-incremental change_fb3
check FB6-compact-A "$build/rig_compact" "$out/backups-fb/$a.zip"
check FB7-refused-while-dj-software-runs refused_while_dj_software_runs
check FB5-cancel-discard "$build/rig_backup" "$B" "$out/backups-fb/$b.zip" --cancel-at 10 discard
check FB4-cancel-keep-resume resume_after_keep
check FB6-compact-B "$build/rig_compact" "$out/backups-fb/$b.zip"
check FB8-restore-A-backup-onto-B "$build/rig_restore" "$out/backups-fb/$a.zip" "$B" --execute
# The round is done with A's archive by now: deleting it is the check.
check FB9-manage-backups-delete "$build/rig_delete_backup" "$out/backups-fb" "$out/backups-fb/$a.zip"
# These two leave the stick changed on purpose -- W8 deletes audio for
# good, F4 fills the stick up -- so they run last, right before the
# restores below put both sticks back at their references.
#
# But B is not at its reference here: FB8 has just restored A's archive
# onto it. W8 cleans up duplicates on B through both catalogs, and on a
# stick carrying A's library over B's files a group's survivor has no
# Device Library Plus row -- which Clean Up deliberately refuses, so W8
# failed every full round for a reason that has nothing to do with it.
# Its first full round (round 4) is where that showed.
check W8-B-at-reference-first "$build/rig_restore" "$refB" "$B" --execute || { echo "stick B could not be put back before W8; stopping"; exit 1; }
check W8-delete-orphans delete_orphans
check F4-stick-fills-up full_stick
check F4-save-fails-after-its-backup full_stick_after_its_backup
check F4-undo-on-a-nearly-full-stick full_stick_undo
# C6 wants a target that B's library does not fit on, and it has to be
# built rather than hoped for: with a 1.2 GiB fixture and a 29 GiB stick
# the check reported "BIG ENOUGH -- this target cannot check C6" and
# failed, having proven nothing. F4's filler is still on A at this point,
# which is the cheapest full stick there will ever be -- filling it again
# after the restores costs twenty minutes over USB 2 for the same state.
# So C6 runs here, and the filler goes immediately afterwards, before the
# restores that would otherwise meet a full stick.
mkdir -p "$out/backups-c6"
# The precondition, asserted rather than assumed: C6 is only a check
# while stick A is still full from F4-undo's filler. Run on its own
# through RIG_ONLY, or after a fill that gave up, A has its whole 29 GiB
# and rig_clone reports "BIG ENOUGH -- this target cannot check C6",
# which is a verdict about the hardware dressed as a result. Say so
# instead, and fail: a check that cannot run has not passed.
c6_needs_a_full_stick() {
    local free_kb; free_kb=$(free_kb_of "$A")
    if [ "$free_kb" -gt 1048576 ]; then
        echo "$A has $free_kb KB free, so no 1.2 GiB library can fail to fit: C6 needs the filler F4-undo leaves"
        echo "behind, and RIG_ONLY must name F4-undo-on-a-nearly-full-stick alongside this check."
        return 1
    fi
    "$build/rig_clone" "$B" "$A" "$out/backups-c6" --expect-too-small
}
check C6-target-too-small c6_needs_a_full_stick
rm -f "$A"/RIG-FILLER-*.bin
sync
echo "filler removed before the restores; $(/bin/df -hP "$A" | awk 'NR==2 {print $4}') free on $A"
check FB-restore-B-from-reference "$build/rig_restore" "$refB" "$B" --execute || { echo "stick B is not back at its reference; stopping"; exit 1; }
check FB-restore-A-from-reference "$build/rig_restore" "$refA" "$A" --execute || { echo "stick A is not back at its reference; stopping"; exit 1; }

# ---- Backup USB Stick between the two --------------------------------
mkdir -p "$out/backups-clone"
check C1-C5-backup-usb-stick "$root/tools/rig-clones.sh" "$A" "$B" "$out/backups-clone" "$refA" "$refB"
# C6 ran earlier, up where stick A was still full -- see the comment there.

# ---- both sticks back where they started -----------------------------
check X2-A-at-reference "$build/rig_restore" "$refA" "$A"
check X2-B-at-reference "$build/rig_restore" "$refB" "$B"
check X1-references-unchanged references_unchanged
# S3 again, at the end: the three checks before it prove nothing about the
# thirty that followed.
check X4-everyday-profile-untouched sandbox_profile_still_clean
# ---- Format USB Stick ------------------------------------------------
#
# The most destructive thing this app does, and until now the only write
# path with no rig row at all. It sits here, after X1/X2 have already
# said both sticks are back at their references, because a format is the
# one check whose subject does not survive it: the partition table goes,
# the filesystem UUID changes, and on a stick whose label decides its
# mount point the path every earlier check used would change too. Nothing
# may depend on stick A after this point.
#
# D1 runs every round. It detects the stick through the app's own
# RemovableMediaLocator, resolves the whole disk behind the partition,
# and proves all four refusals -- no such drive, no whole-disk path, over
# the capacity ceiling, a reference's own directory -- without erasing
# anything. That is the half a round can afford unconditionally, and it
# is a real row: a locator that stops resolving wholeDiskPath fails here.
#
# D2 is the format itself and only runs with RIG_FORMAT_EXECUTE set. It
# formats stick A back to the filesystem and label it already had (read,
# never assumed: see stick_fstype) and then restores it, which costs a
# full restore on top of the round. A round that leaves it off says so
# in the log and writes no result for it at all, so the board cannot keep
# a green D2 from whenever it last ran.
# X3: a file that cannot be read has to become an issue somebody can act
# on. Plants a directory where a track's file should be -- the only way
# to make a read fail that behaves the same on FAT, macOS, Linux and
# Windows -- and puts the file back whatever happens. Runs on B, which is
# already at its reference here, and leaves it there.
check X3-file-failures-as-issues "$build/rig_file_failure" "$B"

check D1-format-preflight "$build/rig_format" "$A" "$(stick_fstype "$A")" "$a"
if [ -n "${RIG_FORMAT_EXECUTE:-}" ]; then
    check D2-format-stick-A "$build/rig_format" "$A" "$(stick_fstype "$A")" "$a" --execute
    # Whatever D2 decided: a stick left empty is worse than a failed
    # format, and the next round starts from the references.
    check D2-restore-A-after-format "$build/rig_restore" "$refA" "$A" --execute
else
    # Deliberately no summary line. The recorder takes PASS or FAIL and
    # nothing else, so a third word here would be dropped on the floor
    # and the board would keep whatever it last said about D2 -- the
    # exact "goes on reading green" failure this rig is built against.
    # D2 is a by-hand row instead, like P1 and P3: set by the person who
    # ran a format round, left alone by every round that did not.
    echo "D2 not run: RIG_FORMAT_EXECUTE is unset, so nothing was erased"
fi

# Last, as its own comment promises: the longest test there is, run once
# everything that touches a stick has finished with it. It needs no stick
# and nothing needs it, so a round loses nothing by ending here.
check S1-corpus corpus

set -f  # a name like F4-* must be reported as typed, not glob-expanded
for wanted in $(rig_only_names); do
    if [[ " $ran " != *" $wanted "* ]]; then
        echo "RIG_ONLY names '$wanted', which is no check of this rig"
        printf '%s\tFAIL\n' "$wanted" >> "$summary"
    fi
done
set +f
if [ ! -s "$summary" ]; then
    echo "no check ran"
    echo "RIG RESULT: FAIL"
    exit 1
fi
echo
cat "$summary"
if grep -q "FAIL" "$summary"; then
    echo "RIG RESULT: FAIL"
    exit 1
fi
echo "RIG RESULT: PASS"
