#!/bin/bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Build an Intel Seabass.app that starts on macOS 12 Monterey, and wrap it
# in a .dmg.
#
#   QT_DIR=<Qt 6.8 macos dir> tools/macos-monterey-dmg.sh <source dir> <work dir> <out.dmg>
#
# QT_DIR is an official Qt 6.8 LTS install (aqt install-qt mac desktop 6.8.3
# clang_64 -m qtmultimedia qtshadertools), e.g. .../6.8.3/macos.
# SEABASS_RELEASE_CHANNEL is passed through to the build (dev when unset).
# SEABASS_SIGN_COMMAND and SEABASS_NOTARIZE_COMMAND work as in
# tools/macos-universal-dmg.sh: the first signs the .app and then the .dmg,
# which must then carry KDE e.V.'s Developer ID; the second notarises the
# .dmg, which must then carry a stapled ticket. Ad hoc otherwise.
#
# Why this exists: the regular packages come from Craft, whose Qt (6.10 and
# later) and every library in its binary cache need macOS 13.3. The Intel
# Macs that stop at Monterey -- the 2017 MacBook Air, for one -- refuse
# them before Seabass can say anything. Qt 6.8 LTS is the last Qt that
# supports macOS 12, Seabass needs no newer, and Qt's own 6.8 binaries are
# built for 12.0. Craft can no longer use an external Qt, and building
# Qt 6.8 inside it means building FFmpeg and everything under it too, so
# this builds the four libraries Seabass needs beside Qt itself, against
# 12.0, and deploys with macdeployqt.
#
# The libraries are built once into <work dir>/deps and reused:
#   - OpenSSL, static, only to be linked into SQLCipher;
#   - SQLCipher 4 (the version and options craft-blueprint/libs/sqlcipher4
#     uses), as libsqlcipher.0.dylib, which Seabass dlopen()s from the
#     bundle's Frameworks;
#   - TagLib, static.
# zlib and SQLite are the system's: both are in every macOS since long
# before 12.
set -u
set -o pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || exit 1
. "$here/macos-bundle-check.sh" || { echo "cannot read $here/macos-bundle-check.sh" >&2; exit 1; }

[ "$#" -eq 3 ] || { sed -n '9,10p' "$0" | sed 's/^# //' >&2; exit 2; }
src="$(cd "$1" && pwd)" || exit 1
work="$2"
# Absolute: ci-notary-service resolves the path it is given from inside
# the file's directory (see tools/macos-universal-dmg.sh).
mkdir -p "$(dirname "$3")" || exit 1
out_dmg="$(cd "$(dirname "$3")" && pwd)/$(basename "$3")" || exit 1
qt="${QT_DIR:?set QT_DIR to a Qt 6.8 macos install}"
[ -x "$qt/bin/macdeployqt" ] || { echo "no macdeployqt under $qt/bin" >&2; exit 1; }
mkdir -p "$work" || exit 1
work="$(cd "$work" && pwd)" || exit 1

target=12.0
arch=x86_64
openssl_version=3.5.9
sqlcipher_version=4.19.0
sqlcipher_sha256=7075f96cbabe45b4ecfc2e6b1745a625f856f695b0827a5506ce9ed85b906aa0
taglib_version=2.1.1

deps="$work/deps"
prefix="$deps/prefix"
export MACOSX_DEPLOYMENT_TARGET="$target"
# Every compile below goes through these, so nothing is built for the
# host by accident: on an Apple Silicon Mac the default is arm64.
cflags="-arch $arch -mmacosx-version-min=$target -O2"
# Same SDK pin as the Craft roots (a stray newer SDK's ld cannot read it).
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
jobs="$(sysctl -n hw.ncpu)"

fetch() {  # <url> <file> [sha256]
    if [ ! -f "$deps/$2" ]; then
        curl -fsSL -o "$deps/$2.part" "$1" || { echo "could not download $1" >&2; return 1; }
        mv "$deps/$2.part" "$deps/$2" || return 1
    fi
    if [ -n "${3:-}" ]; then
        echo "$3  $deps/$2" | shasum -a 256 -c - >/dev/null || { echo "$2 does not match its checksum" >&2; return 1; }
    fi
}

mkdir -p "$deps" "$prefix" || exit 1

if [ ! -f "$prefix/lib/libcrypto.a" ]; then
    echo "== OpenSSL $openssl_version (static, $arch, macOS $target)"
    fetch "https://github.com/openssl/openssl/releases/download/openssl-$openssl_version/openssl-$openssl_version.tar.gz" \
        "openssl-$openssl_version.tar.gz" || exit 1
    rm -rf "$deps/openssl-$openssl_version" && tar -xzf "$deps/openssl-$openssl_version.tar.gz" -C "$deps" || exit 1
    (cd "$deps/openssl-$openssl_version" &&
        ./Configure darwin64-x86_64-cc no-shared no-tests no-docs --prefix="$prefix" --libdir=lib \
            -mmacosx-version-min="$target" >"$deps/openssl.log" 2>&1 &&
        make -j"$jobs" >>"$deps/openssl.log" 2>&1 && make install_sw >>"$deps/openssl.log" 2>&1) ||
        { echo "OpenSSL failed; see $deps/openssl.log" >&2; exit 1; }
fi

if [ ! -f "$prefix/lib/libsqlcipher.0.dylib" ]; then
    echo "== SQLCipher $sqlcipher_version ($arch, macOS $target)"
    fetch "https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v$sqlcipher_version.tar.gz" \
        "sqlcipher-$sqlcipher_version.tar.gz" "$sqlcipher_sha256" || exit 1
    rm -rf "$deps/sqlcipher-$sqlcipher_version" && tar -xzf "$deps/sqlcipher-$sqlcipher_version.tar.gz" -C "$deps" || exit 1
    # The feature set craft-blueprint/libs/sqlcipher4 builds with, so that
    # this package opens exactly what the regular one does.
    features="-DSQLITE_HAS_CODEC -DSQLITE_ENABLE_JSON1 -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_FTS3_PARENTHESIS"
    features="$features -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_COLUMN_METADATA"
    features="$features -DSQLITE_EXTRA_INIT=sqlcipher_extra_init -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown"
    (cd "$deps/sqlcipher-$sqlcipher_version" &&
        CC="clang -arch $arch" CFLAGS="$cflags -I$prefix/include $features" \
        LDFLAGS="-arch $arch -mmacosx-version-min=$target -L$prefix/lib -lcrypto" \
        ./configure --host=x86_64-apple-darwin --prefix="$prefix" --disable-tcl --disable-static \
            --with-tempstore=yes --dll-basename=libsqlcipher >"$deps/sqlcipher.log" 2>&1 &&
        make -j"$jobs" TCLSH_CMD=/usr/bin/tclsh >>"$deps/sqlcipher.log" 2>&1 &&
        make install TCLSH_CMD=/usr/bin/tclsh -j1 >>"$deps/sqlcipher.log" 2>&1) ||
        { echo "SQLCipher failed; see $deps/sqlcipher.log" >&2; exit 1; }
    # Installed under libsqlite3's name, as Craft's recipe finds too.
    for f in "$prefix"/lib/libsqlite3*.dylib; do
        [ -e "$f" ] || continue
        mv "$f" "$prefix/lib/$(basename "$f" | sed 's/sqlite3/sqlcipher/')" || exit 1
    done
    real="$(cd "$prefix/lib" && ls libsqlcipher.*.*.dylib 2>/dev/null | head -1)"
    [ -n "$real" ] || real="$(cd "$prefix/lib" && ls libsqlcipher*.dylib | head -1)"
    [ -f "$prefix/lib/$real" ] && [ ! -L "$prefix/lib/$real" ] || { echo "no SQLCipher library was installed" >&2; exit 1; }
    cp "$prefix/lib/$real" "$prefix/lib/libsqlcipher.0.dylib.tmp" && mv "$prefix/lib/libsqlcipher.0.dylib.tmp" "$prefix/lib/libsqlcipher.0.dylib" || exit 1
    install_name_tool -id "@rpath/libsqlcipher.0.dylib" "$prefix/lib/libsqlcipher.0.dylib" || exit 1
fi

if [ ! -f "$prefix/lib/libtag.a" ]; then
    echo "== TagLib $taglib_version (static, $arch, macOS $target)"
    fetch "https://taglib.org/releases/taglib-$taglib_version.tar.gz" "taglib-$taglib_version.tar.gz" || exit 1
    rm -rf "$deps/taglib-$taglib_version" && tar -xzf "$deps/taglib-$taglib_version.tar.gz" -C "$deps" || exit 1
    cmake -S "$deps/taglib-$taglib_version" -B "$deps/taglib-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$target" -DCMAKE_OSX_SYSROOT="$SDKROOT" \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DBUILD_BINDINGS=OFF \
        -DCMAKE_INSTALL_PREFIX="$prefix" >"$deps/taglib.log" 2>&1 &&
        cmake --build "$deps/taglib-build" >>"$deps/taglib.log" 2>&1 &&
        cmake --install "$deps/taglib-build" >>"$deps/taglib.log" 2>&1 ||
        { echo "TagLib failed; see $deps/taglib.log" >&2; exit 1; }
fi

# Qt 6.8.3's FindWrapOpenGL links Apple's AGL framework, which the macOS 26
# SDK no longer has ("ld: framework 'AGL' not found"); when the SDK lacks
# it, find_library finds the running system's copy instead, which a
# linker pinned to the SDK cannot use either. Later Qt links it only when
# found. Nothing here uses AGL, so it is dropped from this Qt install.
wrap_gl="$qt/lib/cmake/Qt6/FindWrapOpenGL.cmake"
if grep -q "AGL" "$wrap_gl"; then
    echo "== dropping AGL from $wrap_gl"
    perl -0pi -e 's/        find_library\(WrapOpenGL_AGL NAMES AGL\)\n.*?        target_link_libraries\(WrapOpenGL::WrapOpenGL INTERFACE \$\{__opengl_agl_fw_path\}\)\n/        target_link_libraries(WrapOpenGL::WrapOpenGL INTERFACE \$\{__opengl_fw_path\})\n/s' "$wrap_gl" || exit 1
    ! grep -q "AGL" "$wrap_gl" || { echo "could not drop AGL from $wrap_gl" >&2; exit 1; }
fi

echo "== Seabass ($arch, macOS $target, Qt $(basename "$(dirname "$qt")"))"
build="$work/build"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$target" -DCMAKE_OSX_SYSROOT="$SDKROOT" \
    -DCMAKE_PREFIX_PATH="$qt;$prefix" -DCMAKE_FIND_FRAMEWORK=LAST \
    -DSEABASS_TESTS=OFF -DSEABASS_LIBDJINTEROP_TESTS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Boost=ON \
    -DSEABASS_RELEASE_CHANNEL="${SEABASS_RELEASE_CHANNEL:-dev}" >"$work/configure.log" 2>&1 ||
    { echo "configure failed; see $work/configure.log" >&2; exit 1; }
# A TagLib found anywhere but here would be the host's arm64 one.
grep -q "TagLib_DIR:PATH=$prefix" "$build/CMakeCache.txt" ||
    { echo "the build did not find the TagLib built here" >&2; grep TagLib "$build/CMakeCache.txt" >&2; exit 1; }
cmake --build "$build" >"$work/build.log" 2>&1 || { echo "build failed; see $work/build.log" >&2; exit 1; }

echo "== bundling"
# seabass.app, as the build and Craft both name it.
built_app="$(find "$build" -maxdepth 4 -iname 'seabass.app' -type d | head -1)"
[ -d "$built_app" ] || { echo "the build made no seabass.app" >&2; exit 1; }
cli="$(find "$build" -maxdepth 4 -name 'seabass-cli' -type f -perm -u+x | head -1)"
[ -f "$cli" ] || { echo "the build made no seabass-cli" >&2; exit 1; }
app="$work/$(basename "$built_app")"
rm -rf "$app" && cp -R "$built_app" "$app" || exit 1
cp "$cli" "$app/Contents/MacOS/seabass-cli" || exit 1
mkdir -p "$app/Contents/Frameworks" || exit 1
cp "$prefix/lib/libsqlcipher.0.dylib" "$app/Contents/Frameworks/" || exit 1
# Its build left an LC_RPATH into <work>/deps: harmless where that does not
# exist, but a path from the build machine has no business in a package,
# and SQLCipher needs none (system libraries, and OpenSSL linked in).
otool -l "$app/Contents/Frameworks/libsqlcipher.0.dylib" | awk '/LC_RPATH/{r=1} r&&/ path /{print $2; r=0}' |
    while IFS= read -r rpath; do
        install_name_tool -delete_rpath "$rpath" "$app/Contents/Frameworks/libsqlcipher.0.dylib" 2>/dev/null || exit 1
    done || { echo "could not clear SQLCipher's rpaths" >&2; exit 1; }
"$qt/bin/macdeployqt" "$app" -qmldir="$src/src/gui/qml" -executable="$app/Contents/MacOS/seabass-cli" \
    >"$work/macdeployqt.log" 2>&1 || { echo "macdeployqt failed; see $work/macdeployqt.log" >&2; exit 1; }
# macdeployqt deploys every Qt SQL driver it finds, and three of them need
# a database client nobody has: Mimer's, iODBC and PostgreSQL's libpq. Seabass
# opens no database through Qt SQL (QtSql comes in through a QML import),
# so those three drivers are dropped, and the errors about their clients are
# the only ones tolerated.
rm -f "$app"/Contents/PlugIns/sqldrivers/libqsql{mimer,odbc,psql}.dylib || exit 1
if grep -i "^ERROR" "$work/macdeployqt.log" | grep -vE 'libmimerapi|libiodbc|libpq\.' >&2; then
    echo "macdeployqt reported errors" >&2; exit 1
fi

# Qt's binaries are universal; the package is for Intel Macs only, and
# carrying an arm64 slice nobody runs would double its size.
echo "== thinning to $arch"
thinned=0
while IFS= read -r f; do
    archs="$(lipo -archs "$f" 2>/dev/null)" || continue
    case "$archs" in
        "$arch") : ;;
        *"$arch"*) lipo -thin "$arch" "$f" -output "$f.thin" && mv "$f.thin" "$f" || exit 1; thinned=$((thinned+1)) ;;
        *) echo "  NO $arch SLICE: ${f#$app/} ($archs)" >&2; exit 1 ;;
    esac
done < <(find "$app" -type f)
echo "  $thinned thinned"

# The check that matters: macOS refuses a binary whose minimum is newer
# than itself, and the dialog it shows is the whole failure this exists
# for. Read from every Mach-O, not assumed from the flags above.
echo "== every Mach-O runs on macOS $target"
newer=0; total=0
while IFS= read -r f; do
    lipo -archs "$f" >/dev/null 2>&1 || continue
    total=$((total+1))
    minos="$(otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit} /LC_VERSION_MIN_MACOSX/{v=1} v&&/version/{print $2; exit}')"
    if [ -z "$minos" ] || [ "$(printf '%s\n%s\n' "$minos" "$target" | sort -V | tail -1)" != "$target" ]; then
        echo "  ${f#$app/}: minimum macOS ${minos:-unknown}" >&2; newer=$((newer+1))
    fi
done < <(find "$app" -type f)
echo "  $total Mach-O files, $newer needing a newer macOS"
[ "$newer" -eq 0 ] || exit 1

plist="$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $target" "$plist" 2>/dev/null ||
    /usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string $target" "$plist" || exit 1

echo "== the bundle is self-contained"
seabass_clear_dyld_env
seabass_bundle_self_contained "$app" || exit 1

sign_cmd=(); notarize_cmd=()
[ -n "${SEABASS_SIGN_COMMAND:-}" ] && read -r -a sign_cmd <<< "$SEABASS_SIGN_COMMAND"
[ -n "${SEABASS_NOTARIZE_COMMAND:-}" ] && read -r -a notarize_cmd <<< "$SEABASS_NOTARIZE_COMMAND"
echo "== signing"
if [ "${#sign_cmd[@]}" -gt 0 ]; then
    "${sign_cmd[@]}" "$app" || { echo "  the signing command failed" >&2; exit 1; }
    seabass_really_signed "$app" || { echo "  the bundle is not signed for distribution after signing" >&2; exit 1; }
else
    # Innermost first: a framework's CodeResources hashes the binaries
    # thinning just replaced.
    while IFS= read -r fw; do
        codesign --force --sign - --timestamp=none "$fw" >/dev/null 2>&1 || { echo "  could not sign $fw" >&2; exit 1; }
    done < <(find "$app" -name '*.framework' -type d | sort -r)
    while IFS= read -r dylib; do
        codesign --force --sign - --timestamp=none "$dylib" >/dev/null 2>&1 || { echo "  could not sign $dylib" >&2; exit 1; }
    done < <(find "$app" -name '*.dylib' -type f)
    codesign --force --deep --sign - --timestamp=none "$app" >/dev/null 2>&1 || { echo "  could not sign the bundle" >&2; exit 1; }
fi
codesign --verify --deep --strict "$app" || { echo "  the bundle does not verify" >&2; exit 1; }
echo "  verifies"

# Run, under Rosetta on Apple Silicon: a bundle that loads nothing it
# lacks still has to start.
echo "== the bundle starts"
arch -x86_64 "$app/Contents/MacOS/seabass-cli" --help >/dev/null 2>&1 </dev/null ||
    { echo "  seabass-cli does not run" >&2; exit 1; }
echo "  seabass-cli runs"

echo "== building $out_dmg"
rm -f "$out_dmg" || exit 1
staging="$work/dmg"
rm -rf "$staging" && mkdir -p "$staging" || exit 1
cp -R "$app" "$staging/" || exit 1
ln -s /Applications "$staging/Applications" || exit 1
hdiutil create -volname "Seabass" -srcfolder "$staging" -fs HFS+ -ov -format UDZO -quiet "$out_dmg" || exit 1
if [ "${#sign_cmd[@]}" -gt 0 ]; then
    echo "== signing the image"
    "${sign_cmd[@]}" "$out_dmg" || { echo "  the signing command failed" >&2; exit 1; }
    seabass_really_signed "$out_dmg" || { echo "  the image is not signed for distribution after signing" >&2; exit 1; }
fi
if [ "${#notarize_cmd[@]}" -gt 0 ]; then
    echo "== notarising the image"
    "${notarize_cmd[@]}" "$out_dmg" || { echo "  the notarising command failed" >&2; exit 1; }
    xcrun stapler validate "$out_dmg" || { echo "  no notarisation ticket is stapled to the image" >&2; exit 1; }
fi
# The bare filename, so `shasum -c` works wherever it is downloaded to.
( cd "$(dirname "$out_dmg")" && shasum -a 256 "$(basename "$out_dmg")" | tee "$(basename "$out_dmg").sha256" &&
    shasum -a 256 -c "$(basename "$out_dmg").sha256" >/dev/null ) ||
    { echo "  could not write a checksum that checks" >&2; exit 1; }
echo "done"
