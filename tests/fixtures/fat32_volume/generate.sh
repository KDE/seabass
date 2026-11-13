#!/bin/sh
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: CC0-1.0

# Makes fat32-64mib.img.gz, the volume damage_filesystem_test damages on
# every machine, including one that has no mkfs.fat of its own. See
# README.txt.
#
# Needs mkfs.fat and fsck.fat (dosfstools) and mcopy and mmd (mtools).
# None of it needs root.
set -eu

out="${1:-$(dirname "$0")/fat32-64mib.img.gz}"
PATH="$PATH:/usr/sbin:/sbin"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

image="$work/fat32-64mib.img"
dd if=/dev/zero of="$image" bs=1M count=64 status=none
# A fixed volume id and --invariant, so two runs of the same dosfstools
# make the same filesystem.
mkfs.fat -F 32 -n DAMAGETEST -i 5EABA550 --invariant "$image" > /dev/null

# Files and a directory, so "no other byte changed" is a claim about a
# volume with directory entries and file data in it.
printf 'a track' > "$work/track1.mp3"
printf 'another' > "$work/track2.mp3"
seq 1 2000 > "$work/numbers.txt"
export SOURCE_DATE_EPOCH=1794528000
mcopy -i "$image" "$work/track1.mp3" ::track1.mp3
mcopy -i "$image" "$work/track2.mp3" ::track2.mp3
mmd -i "$image" ::Contents
mcopy -i "$image" "$work/numbers.txt" ::Contents/numbers.txt

# The volume must start out clean, or the test's "fsck reports the
# damage" would be true before anything was damaged.
fsck.fat -n "$image" > /dev/null

gzip -9 -n -c "$image" > "$out"
echo "$out: $(wc -c < "$out") bytes; sha256 of the image inside: $(sha256sum "$image" | cut -d' ' -f1)"
