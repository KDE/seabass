#!/bin/bash
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# What to run before every push: build, then the WHOLE suite. CI on a push
# runs only the C++ tests that need no display and no clock (CONTRIBUTING.md,
# "What CI runs"); the QML suites, the desktop-style lane, the storm, the
# `integration` and `timing` tests run here, on your machine, or nowhere
# until the nightly.
#
#   tools/pre-push-check.sh [-B build-dir] [-j jobs] [-R ctest-regex] [-- extra cmake args]
#
#   -B  build directory (default: build-pre-push in the checkout). An
#       existing one is reused; it is reconfigured with the storm and the
#       QML suites switched on, so its other settings stay as they were.
#   -j  build and ctest parallelism (default: the number of cores)
#   -R  run only the tests matching this ctest regex, for a quick check of
#       the script or of one test. A filtered run is not a pre-push check
#       and says so.
#
# On Linux the suite runs under `xvfb-run -a`, so nothing it opens lands
# on your desktop (the QML suites start their own xvfb besides). Set
# SEABASS_PRE_PUSH_DISPLAY=1 to run on your own display instead. macOS
# has no xvfb and needs none.
#
# Exit status: 0 only when every test passed and none was skipped. A skip
# is a failure in this project (docs/testing.md): a test that did not run
# proves nothing, whatever colour it shows.

set -euo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$source_dir/build-pre-push"
filter=""
case "$(uname -s)" in
    Darwin) jobs="$(sysctl -n hw.ncpu)" ;;
    *) jobs="$(nproc)" ;;
esac

while getopts "B:j:R:h" opt; do
    case "$opt" in
        B) build_dir="$OPTARG" ;;
        j) jobs="$OPTARG" ;;
        R) filter="$OPTARG" ;;
        h) sed -n '6,29p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ "${1:-}" = "--" ] && shift

started=$(date +%s)
say() { printf '\n== %s\n' "$*"; }

say "configure $build_dir"
configure=(cmake -S "$source_dir" -B "$build_dir" -DSEABASS_TESTS=ON -DSEABASS_QML_TESTS=ON -DSEABASS_STORM_TESTS=ON "$@")
if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    configure+=(-G Ninja -DCMAKE_BUILD_TYPE=Debug)
fi
"${configure[@]}"

say "build (-j$jobs)"
cmake --build "$build_dir" -j "$jobs"

run=()
if [ "$(uname -s)" = "Linux" ] && [ "${SEABASS_PRE_PUSH_DISPLAY:-}" != "1" ]; then
    if ! command -v xvfb-run >/dev/null; then
        echo "xvfb-run not found (Debian/Ubuntu: xvfb). Install it, or set SEABASS_PRE_PUSH_DISPLAY=1 to use your display." >&2
        exit 1
    fi
    run=(xvfb-run -a -s "-screen 0 1280x1024x24")
fi

# The desktop-style QML lane is registered only where org.kde.desktop is
# installed, and a lane that is not registered is a lane that did not run.
if [ "$(uname -s)" = "Linux" ] && [ -z "$filter" ]; then
    # grep reads to the end (no -q): with -q it stops at the first match,
    # ctest dies of SIGPIPE, and pipefail turns a registered lane into
    # "not registered".
    if ! (cd "$build_dir" && ctest -N) | grep "desktop_style" >/dev/null; then
        echo "The desktop-style QML lane is not registered in $build_dir: install kf6-qqc2-desktop-style" >&2
        echo "(the style the Linux build ships with) and run this again." >&2
        exit 1
    fi
fi

ctest_args=(-j "$jobs" --output-on-failure --no-tests=error)
[ -n "$filter" ] && ctest_args+=(-R "$filter")
log="$build_dir/pre-push-ctest.log"
say "ctest ${ctest_args[*]}${run:+ (under ${run[0]})}"
status=0
(cd "$build_dir" && ${run[@]+"${run[@]}"} ctest "${ctest_args[@]}") 2>&1 | tee "$log" || status=$?

# Skips: ctest's own (a SKIP_RETURN_CODE, a test that could not start),
# and QtTest's inside a passing executable, which only the full log shows.
skipped_tests="$(grep -E '\*\*\*(Skipped|Not Run)|\((Skipped|Not Run|Disabled)\)' "$log" || true)"
skipped_cases="$(grep -E '^SKIP +:' "$build_dir/Testing/Temporary/LastTest.log" 2>/dev/null || true)"
summary="$(grep -E 'tests passed, [0-9]+ tests failed out of' "$log" || echo "no ctest summary line")"
elapsed=$(( $(date +%s) - started ))

say "summary"
echo "$summary"
echo "time: $((elapsed / 60)) min $((elapsed % 60)) s"
verdict=0
if [ "$status" -ne 0 ]; then
    echo "FAIL: ctest exited $status"
    verdict=1
fi
if [ -n "$skipped_tests" ]; then
    echo "FAIL: tests that did not run:"
    echo "$skipped_tests"
    verdict=1
fi
if [ -n "$skipped_cases" ]; then
    echo "FAIL: skipped test cases:"
    echo "$skipped_cases"
    verdict=1
fi
if [ "$verdict" -eq 0 ]; then
    if [ -n "$filter" ]; then
        echo "PASS, filtered to '$filter': fine for a quick look, not a pre-push check"
    else
        echo "PASS: build current, every test ran and passed"
    fi
fi
exit "$verdict"
