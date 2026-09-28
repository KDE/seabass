#!/bin/bash
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# A big storm (docs/testing.md, "The storm") in several processes at once:
# seeds FIRST .. FIRST+COUNT-1, split into JOBS disjoint ranges, each run by
# seabass_qml_tests under its own xvfb-run with the environment ctest gives
# seabass_qml_storm_tests (read from `ctest -N -V`, so the two cannot
# drift), plus a SEABASS_HOME and settings of its own per job.
#
# It runs from a frozen copy of the test binary and of tests/qml-storm, so
# a rebuild or an edit meanwhile never lands in the middle of a hunt.
#
#   tools/storm-hunt.sh <build-dir> <out-dir> [first] [count] [jobs] [steps]
#
# Every failing seed ends up in <out-dir>/failures.txt, one line each, and
# its steps in the job's log. Exit status: 0 when every seed passed.

set -u

build=${1:?build directory}
out=${2:?output directory}
first=${3:-1}
count=${4:-200}
jobs=${5:-4}
steps=${6:-150}

src=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$build/CMakeCache.txt")
if [ -z "$src" ] || [ ! -x "$build/seabass_qml_tests" ]; then
    echo "storm-hunt: $build is not a Seabass build directory with seabass_qml_tests in it" >&2
    exit 2
fi

mkdir -p "$out"
: > "$out/failures.txt"
: > "$out/jobs.txt"

# The environment ctest runs the storm with, and nothing of this shell's
# that could let a real stick in: SEABASS_IGNORE_REMOVABLE_MEDIA must be in it.
env_file="$out/storm.env"
(cd "$build" && ctest -N -V -R '^seabass_qml_storm_tests$') \
    | sed -n '/Environment variables:/,/Test #/p' | sed -n 's/^[0-9]*:  //p' > "$env_file"
if ! grep -q '^SEABASS_IGNORE_REMOVABLE_MEDIA=1$' "$env_file"; then
    echo "storm-hunt: seabass_qml_storm_tests does not run with SEABASS_IGNORE_REMOVABLE_MEDIA=1; refusing" >&2
    exit 2
fi

snap="$out/snap"
rm -rf "$snap"
mkdir -p "$snap/tests"
cp "$build/seabass_qml_tests" "$snap/seabass_qml_tests"
cp -r "$src/tests/qml-storm" "$snap/tests/qml-storm"
ln -s "$src/tests/fixtures" "$snap/tests/fixtures"

per=$(( (count + jobs - 1) / jobs ))
for job in $(seq 0 $((jobs - 1))); do
    from=$((first + job * per))
    n=$per
    if [ $((from + n)) -gt $((first + count)) ]; then
        n=$((first + count - from))
    fi
    [ "$n" -le 0 ] && continue
    (
        set -a
        # shellcheck disable=SC1090
        . "$env_file"
        set +a
        unset WAYLAND_DISPLAY
        export SEABASS_HOME="$out/home-$job" XDG_CONFIG_HOME="$out/config-$job" APPDATA="$out/config-$job"
        mkdir -p "$SEABASS_HOME" "$XDG_CONFIG_HOME"
        export SEABASS_STORM_FIRST_SEED=$from SEABASS_STORM_SEEDS=$n SEABASS_STORM_STEPS=$steps
        export SEABASS_STORM_FAILURES="$out/failures.txt"
        # A seed takes about a minute in a Debug build; a whole range gets
        # three minutes a seed before it counts as stuck.
        timeout $((n * 180 + 300)) xvfb-run -a -s "-screen 0 1280x1024x24" \
            "$snap/seabass_qml_tests" -input "$snap/tests/qml-storm" > "$out/job-$job.log" 2>&1
        status=$?
        echo "job $job (seeds $from to $((from + n - 1))): exit $status" >> "$out/jobs.txt"
        # QuickTest exits with its failure count; anything past that is the
        # process dying (a crash, an abort, the timeout), in the seed it was on.
        if [ "$status" -gt 100 ]; then
            last=$(grep -o '^STORM seed [0-9]*:' "$out/job-$job.log" | tail -1 | grep -o '[0-9]*')
            echo "seed ${last:-?}: the process died (exit $status); the rest of seeds $from to $((from + n - 1)) did not run" \
                >> "$out/failures.txt"
        fi
    ) &
    # xvfb-run -a picks a free display and then takes it, not in one step.
    sleep 5
done
wait

cat "$out/jobs.txt"
passed=$(cat "$out"/job-*.log | grep -c '^STORM seed [0-9]* passed')
failed=$(grep -c . "$out/failures.txt")
echo "storm-hunt: $passed passed, $failed failed of $count seeds, $steps steps each"
[ "$failed" -eq 0 ] && [ "$passed" -eq "$count" ]
