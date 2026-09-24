# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# What release this build is, from the tag it was triggered by.
#
# Sourced by CI and by tools/release.sh. Sets SEABASS_CHANNEL and
# SEABASS_VERSION from a "releases/<channel>/X.Y.Z" tag, and refuses a
# tag whose version disagrees with CMakeLists.txt -- one version, written
# in one place, and the tag says which commit of it is the release.
#
# Without a tag it sets channel "dev" and takes the version from
# CMakeLists.txt, which is what a build from a working tree is.

seabass_project_version() {  # <repo root>
    sed -n 's/^project(seabass VERSION \([0-9][0-9.]*\).*/\1/p' "$1/CMakeLists.txt" | head -1
}

# Writes X.Y.Z into project(seabass VERSION ...). A temporary file and a
# rename rather than sed -i, whose syntax differs between GNU and BSD sed
# and so between Linux and the Mac. Fails when the line is not there, or
# when the file does not say the new version afterwards.
seabass_set_project_version() {  # <repo root> <X.Y.Z>
    local file="$1/CMakeLists.txt"
    sed "s/^\(project(seabass VERSION \)[0-9][0-9.]*/\1$2/" "$file" > "$file.version-tmp" \
        && mv "$file.version-tmp" "$file" \
        && [ "$(seabass_project_version "$1")" = "$2" ]
}

seabass_release_from_tag() {  # <repo root> <tag or empty>
    local root="$1"
    local tag="${2:-}"
    local declared
    declared="$(seabass_project_version "$root")"
    if [ -z "$declared" ]; then
        echo "no version in $root/CMakeLists.txt: expected project(seabass VERSION X.Y.Z ...)" >&2
        return 1
    fi
    if [ -z "$tag" ]; then
        SEABASS_CHANNEL="dev"
        SEABASS_VERSION="$declared"
        return 0
    fi
    case "$tag" in
        releases/alpha/*|releases/beta/*|releases/stable/*) ;;
        *)
            echo "$tag is not a release tag: expected releases/<alpha|beta|stable>/X.Y.Z" >&2
            return 1
            ;;
    esac
    SEABASS_CHANNEL="${tag#releases/}"
    SEABASS_CHANNEL="${SEABASS_CHANNEL%%/*}"
    SEABASS_VERSION="${tag##*/}"
    case "$SEABASS_VERSION" in
        [0-9]*.[0-9]*.[0-9]*) ;;
        *)
            echo "$SEABASS_VERSION is not a X.Y.Z version" >&2
            return 1
            ;;
    esac
    if [ "$SEABASS_VERSION" != "$declared" ]; then
        # The one mistake this whole scheme exists to prevent: a tag that
        # says 0.3.0 on a tree that builds 0.2.0, and packages, installers
        # and the update check all disagreeing about what people have.
        echo "tag says $SEABASS_VERSION, CMakeLists.txt says $declared" >&2
        echo "bump project(seabass VERSION ...) and commit before tagging" >&2
        return 1
    fi
    return 0
}

# seabass-0.2.1_beta_linux.tar.gz -- the name the website and
# ~/Seabass/releases/<channel>/ both use.
seabass_package_name() {  # <version> <channel> <os> <extension>
    printf 'seabass-%s_%s_%s.%s' "$1" "$2" "$3" "$4"
}
