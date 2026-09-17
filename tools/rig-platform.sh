# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Sourced by the rig scripts (rig-shakedown.sh, rig-edits.sh, rig-clones.sh,
# tests/qml-live/run-live.sh): the few things that differ between the Linux
# rig machine and a Mac, so the scripts themselves stay one program.
#
# macOS: the scripts use GNU stat/xargs/readlink/dd/stdbuf options, so
# Homebrew's coreutils and findutils go first on PATH (brew install
# coreutils findutils). A sandbox that lacks them fails here, loudly,
# rather than halfway through a round on a BSD option.

rig_os="$(uname -s)"

if [ "$rig_os" = "Darwin" ]; then
    # Homebrew's own bin too: cmake and ctest live there, not in /usr/bin,
    # and a round started from a shell without it failed its first check
    # ("ctest: command not found") having built nothing.
    for brewbin in /opt/homebrew/bin /usr/local/bin; do
        case ":$PATH:" in
            *":$brewbin:"*) ;;
            *) [ -d "$brewbin" ] && PATH="$brewbin:$PATH" ;;
        esac
    done
    for gnubin in /opt/homebrew/opt/coreutils/libexec/gnubin /opt/homebrew/opt/findutils/libexec/gnubin \
                  /usr/local/opt/coreutils/libexec/gnubin /usr/local/opt/findutils/libexec/gnubin; do
        [ -d "$gnubin" ] && PATH="$gnubin:$PATH"
    done
    # And the system's own sbin, which holds diskutil and mount. A round
    # started from a stripped environment had them missing: stick_device()
    # returned the empty string, the live bundle was handed no device and
    # refused to start, and stick_uuid() would have compared one empty
    # string with another and called it a match.
    for sysbin in /usr/sbin /sbin; do
        case ":$PATH:" in
            *":$sysbin:"*) ;;
            *) PATH="$PATH:$sysbin" ;;
        esac
    done
    export PATH
    # Both packages, checked by what the scripts use: BSD xargs rejects -d,
    # and a catalog listing that comes out empty would make every catalog
    # comparison of a round pass having compared nothing.
    if ! command -v ctest >/dev/null; then
        echo "rig-platform.sh: ctest is not on PATH (brew install cmake)" >&2
        exit 1
    fi
    if ! command -v diskutil >/dev/null; then
        # Every stick fact on macOS comes through diskutil: the device
        # node, the volume UUID, unmounting and mounting again. Without it
        # those helpers return the empty string, which is not an error
        # anywhere it is used -- an empty UUID compares equal to an empty
        # UUID, and a check that compared nothing would be recorded green.
        echo "rig-platform.sh: diskutil is not on PATH (expected in /usr/sbin)" >&2
        exit 1
    fi
    if ! stat -c %s / >/dev/null 2>&1 || ! command -v stdbuf >/dev/null \
        || ! xargs --version 2>/dev/null | grep -q GNU || ! find --version 2>/dev/null | grep -q GNU; then
        echo "rig-platform.sh: GNU coreutils/findutils are needed on macOS (brew install coreutils findutils)" >&2
        exit 1
    fi
    # Every rig script may be pointed at a mounted disk image standing in
    # for a stick (docs/testing.md); the app lists one only with this set.
    export SEABASS_ACCEPT_DISK_IMAGES=1
fi

# True on a Windows shell (MSYS2, git-bash, Cygwin), where `uname -s`
# reports MINGW64_NT-10.0 and the like.
rig_is_windows() {
    case "$rig_os" in
        MINGW*|MSYS*|CYGWIN*) return 0 ;;
        *) return 1 ;;
    esac
}

# Where the app really keeps its settings, listed so that a change to
# them is visible to a diff.
#
# Linux writes a file under XDG_CONFIG_HOME and macOS a property list,
# both of which a file listing can see. Windows writes the registry --
# gui/seabass_settings.hpp's own comment records the measurement: on
# Windows a real run resolves to HKEY_CURRENT_USER\Software\seabass\
# seabass whatever XDG_CONFIG_HOME says, because Qt does not read it
# there. So on Windows the file listing finds nothing, compares nothing,
# and reports agreement: a guard that cannot fail. This prints the key
# instead, and says so loudly when it cannot.
everyday_settings_listing() {
    if ! rig_is_windows; then
        return 0
    fi
    if command -v reg >/dev/null 2>&1; then
        # The reading is taken first and tested on its own. Piping reg
        # straight into sed puts sed's status (always 0) on the pipeline,
        # so `|| echo ABSENT` could never fire: an absent key printed
        # NOTHING, before and after, and the diff agreed with itself --
        # the very guard-that-cannot-fail this function exists to remove.
        local dump
        if dump=$(reg query 'HKCU\Software\seabass\seabass' /s 2>/dev/null); then
            printf '%s\n' "$dump" | sed -e 's/[[:space:]]*$//' -e '/^$/d'
        else
            echo 'HKCU\Software\seabass\seabass ABSENT'
        fi
    else
        echo 'HKCU\Software\seabass\seabass UNWATCHED-NO-REG-EXE'
    fi
}

# Non-zero when the line above is a placeholder rather than a reading, so
# a check can fail instead of comparing two placeholders and passing.
everyday_settings_watchable() {
    if ! rig_is_windows; then
        return 0
    fi
    command -v reg >/dev/null 2>&1
}

# The filesystem UUID the app keys a stick's edit lock on: udev's ID_FS_UUID
# on Linux, DiskArbitration's volume UUID on macOS (what `diskutil info`
# prints as "Volume UUID").
stick_uuid() {  # <mount point>
    if [ "$rig_os" = "Darwin" ]; then
        diskutil info "$1" 2>/dev/null | awk -F': *' '/^ *Volume UUID:/ {print $2; exit}'
    else
        lsblk -no UUID "$(findmnt -no SOURCE "$1")" 2>/dev/null | head -1
    fi
}

# The device node behind a mount point (/dev/sdc1, /dev/disk6s1).
stick_device() {  # <mount point>
    if [ "$rig_os" = "Darwin" ]; then
        diskutil info "$1" 2>/dev/null | awk -F': *' '/^ *Device Node:/ {print $2; exit}'
    else
        findmnt -no SOURCE "$1" 2>/dev/null
    fi
}

unmount_device() {  # <device>
    if [ "$rig_os" = "Darwin" ]; then
        diskutil unmount "$1" >/dev/null 2>&1
    else
        udisksctl unmount -b "$1" >/dev/null 2>&1
    fi
}

# Puts a device back after unmount_device(). On macOS a disk image standing
# in for a stick disappears entirely when its last volume is unmounted --
# the image detaches, unlike a real stick, and its /dev node is gone -- so
# the images named in RIG_STICK_IMAGES (space separated .sparseimage paths)
# are re-attached before the plain mount is tried.
mount_device() {  # <device>
    if [ "$rig_os" = "Darwin" ]; then
        if [ ! -e "$1" ]; then
            for image in ${RIG_STICK_IMAGES:-}; do
                [ -f "$image" ] && hdiutil attach -nobrowse "$image" >/dev/null 2>&1
            done
        fi
        diskutil mount "$1" >/dev/null 2>&1 || [ -e "$1" ]
    else
        udisksctl mount -b "$1" >/dev/null 2>&1
    fi
}

# What infrastructure::system::currentProcessStartId() records for a
# process: /proc's start time in clock ticks on Linux, the start time in
# whole seconds on macOS.
process_start_id() {  # <pid>
    if [ "$rig_os" = "Darwin" ]; then
        local started
        # In UTC on both sides: local time repeats an hour when summer time
        # ends, and the conversion back could land an hour off.
        started="$(TZ=UTC LC_ALL=C ps -o lstart= -p "$1" | sed 's/  *$//')"
        [ -n "$started" ] && TZ=UTC LC_ALL=C /bin/date -j -f "%a %b %d %T %Y" "$started" +%s 2>/dev/null
    else
        awk '{print $22}' "/proc/$1/stat"
    fi
}
