A FAT32 volume for damage_filesystem_test: 64 MiB, label DAMAGETEST, two
files in the root and one in a directory. Made by mkfs.fat (dosfstools
4.2) and filled by mtools 4.0.49 in an ubuntu:26.04 container on
2026-11-13, by generate.sh in this directory, and gzipped, since nearly
all of it is zeroes.

It is here so the test can hold the damage command to its promise on a
machine that has no mkfs.fat, which is KDE's CI image: the test used to
skip there. A volume made by code in this repository would not do; the
point is that the offsets come out right on a filesystem somebody else
laid out. Where mkfs.fat is installed the test makes a fresh volume as
well.

generate.sh makes the same bytes again with the same dosfstools and
mtools:

    docker run --rm -v "$PWD/tests/fixtures/fat32_volume:/work" ubuntu:26.04 sh -c \
        "apt-get update -qq && apt-get install -y -qq dosfstools mtools && /work/generate.sh"

sha256 of the image inside the .gz:
67d9b3027558babcba34fdf0f8e87d4af651ebaf8685947ae4bad4b1a4630f25
