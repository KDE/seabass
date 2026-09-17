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
    for gnubin in /opt/homebrew/opt/coreutils/libexec/gnubin /opt/homebrew/opt/findutils/libexec/gnubin \
                  /usr/local/opt/coreutils/libexec/gnubin /usr/local/opt/findutils/libexec/gnubin; do
        [ -d "$gnubin" ] && PATH="$gnubin:$PATH"
    done
    export PATH
    if ! stat -c %s / >/dev/null 2>&1 || ! command -v stdbuf >/dev/null; then
        echo "rig-platform.sh: GNU coreutils/findutils are needed on macOS (brew install coreutils findutils)" >&2
        exit 1
    fi
fi

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

mount_device() {  # <device>
    if [ "$rig_os" = "Darwin" ]; then
        diskutil mount "$1" >/dev/null 2>&1
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
        started="$(LC_ALL=C ps -o lstart= -p "$1" | sed 's/  *$//')"
        [ -n "$started" ] && LC_ALL=C /bin/date -j -f "%a %b %d %T %Y" "$started" +%s 2>/dev/null
    else
        awk '{print $22}' "/proc/$1/stat"
    fi
}
