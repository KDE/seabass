# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Sourced by rig-shakedown.sh and rig-interrupted-save.sh: the catalog
# files of a stick, what they say, and whether they are still what a
# baseline recorded. Moved here from rig-shakedown.sh unchanged, so F6's
# standalone run compares exactly the way a round does.
#
# The caller sets, before calling unchanged_catalogs:
#   build                         the build, for seabass-cli
#   rig_catalog_baseline          sha256sum lines from catalogs(), per stick
#   rig_catalog_digest_baseline   catalog_digests() lines, taken at the same moment

# What the catalogs SAY, as opposed to the bytes catalogs() hashes.
#
# A byte comparison cannot tell a SQLite checkpoint from a real change --
# folding the write-ahead log in rewrites the database, so both look the
# same -- and exportLibrary.db is SQLCipher-encrypted, so nothing outside
# Seabass can read it to find out. seabass-cli digest reads each catalog
# with the reader the app itself uses and prints "<name>\t<sha256>" over
# the tracks, their authored metadata and their cues.
#
# Never fails the round on its own: a round must not go red because this
# could not run. It is the second opinion asked for when the bytes
# disagree, and unchanged_catalogs says so plainly when there is none.
catalog_digests() {  # stick -> "<name>\t<sha256>" lines, or nothing
    [ -x "$build/seabass-cli" ] || return 0
    "$build/seabass-cli" digest --rekordbox "$1/PIONEER" 2>/dev/null
    [ -d "$1/Engine Library/Database2" ] && "$build/seabass-cli" digest --engine "$1/Engine Library" 2>/dev/null
    return 0
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
                    if grep -F "$1/" "$rig_catalog_baseline" \
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
        elif [ ! -e "$file-wal" ] && grep -F "$1/" "$rig_catalog_baseline" \
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
            # The bytes cannot answer this one, so ask the catalogs.
            # A checkpoint rewrites the database, so differing bytes are
            # expected here and say nothing either way; what decides it
            # is whether the library still says the same thing, which
            # only Seabass can read out of a SQLCipher database.
            local digests_now; digests_now=$(catalog_digests "$1")
            local matched=0 compared=0
            # Its own name and digest: without `local` this loop's `name`
            # is check()'s, up the call chain (bash scopes dynamically), and
            # the check that reached this branch was recorded under an
            # empty name (round 10, F5: "--- : FAIL", a summary row with no
            # check, and the board never heard of it).
            local name digest
            while IFS=$'\t' read -r name digest; do
                [ -n "$name" ] || continue
                local was; was=$(awk -F'\t' -v n="$name" '$1 == n {print $2}' \
                    "$rig_catalog_digest_baseline" 2>/dev/null)
                [ -n "$was" ] || continue
                compared=$((compared + 1))
                [ "$was" = "$digest" ] && matched=$((matched + 1))
            done <<< "$digests_now"

            if [ "$compared" -gt 0 ] && [ "$matched" -eq "$compared" ]; then
                # Not a failure. The file moved and the library did not,
                # which is exactly what a checkpoint is.
                echo "$file: bytes differ, but its write-ahead log was checkpointed into it and all"
                echo "    $compared catalog(s) read back identical to the baseline. A checkpoint rewrites"
                echo "    the database without changing what it says, so this is the file moving, not the"
                echo "    library. Compared by content because bytes cannot tell the two apart."
            elif [ "$compared" -gt 0 ]; then
                echo "$file: DIFFERS, and so does what the catalog SAYS: $matched of $compared read back"
                echo "    identical to the baseline. This is not a checkpoint -- the library changed."
                bad=1
            else
                echo "$file: DIFFERS, and its write-ahead log was checkpointed into it."
                echo "    A checkpoint rewrites the database, so differing bytes are expected here, and"
                echo "    no content digest was available to tell that from a real change (is seabass-cli"
                echo "    built in $build?). Read the undo's own lines in the stick log above."
                bad=1
            fi
        else
            echo "$file: DIFFERS"
            bad=1
        fi
    done < <(grep -F "$1/" "$rig_catalog_baseline")
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
