# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: BSD-2-Clause

import info
from CraftCore import CraftCore


class subinfo(info.infoclass):
    def setTargets(self):
        self.displayName = "Seabass"
        self.description = "Syncs cue points between rekordbox and Denon Engine DJ libraries"
        self.webpage = "https://invent.kde.org/multimedia/seabass"
        self.svnTargets["master"] = "https://invent.kde.org/multimedia/seabass.git|master"
        self.defaultTarget = "master"

    def setDependencies(self):
        self.buildDependencies["dev-utils/cmake"] = None
        self.buildDependencies["dev-utils/ninja"] = None
        # Needed to compile QML's shader effects; not confirmed whether
        # qtdeclarative already pulls this in transitively, listed
        # explicitly to be safe.
        self.buildDependencies["libs/qt6/qtshadertools"] = None
        self.runtimeDependencies["libs/qt/qtbase"] = None
        # Quick, QuickControls2, QuickLayouts and QuickDialogs2 are all
        # part of this one module in Qt6 -- confirmed directly, none of
        # them exist as separate Craft blueprints the way they might on
        # a system package manager.
        self.runtimeDependencies["libs/qt/qtdeclarative"] = None
        self.runtimeDependencies["libs/qt/qtmultimedia"] = None
        self.runtimeDependencies["libs/qt/qtsvg"] = None
        self.runtimeDependencies["libs/zlib"] = None
        self.runtimeDependencies["libs/sqlite"] = None
        # Loaded via LoadLibrary/dlopen at runtime, never linked (see
        # sqlcipher_dyn.hpp in Seabass's own source for why) -- listed
        # as a runtime dependency for exactly that reason, so Craft's
        # packaging step still bundles the DLL even though nothing in
        # the CMake build itself links against it.
        self.runtimeDependencies["libs/sqlcipher"] = None
        # Optional: Seabass's own CMakeLists.txt falls back to
        # application::NullTrackMetadataProbe when TagLib isn't found.
        self.runtimeDependencies["libs/taglib"] = None


from Package.CMakePackageBase import CMakePackageBase


class Package(CMakePackageBase):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        # CMakePackageBase passes -DBUILD_TESTING=ON by default, which
        # reaches libdjinterop's own vendored CMakeLists.txt (a git
        # submodule) too -- its test suite needs Boost.Filesystem, and
        # Craft only provides the Boost headers, not the compiled
        # library. Confirmed directly: configure failed on exactly this
        # ("libdjinterop's test suite needs Boost.Filesystem..."), and
        # Seabass's own CMakeLists.txt documents this exact flag for it.
        # SEABASS_TESTS=OFF alongside it for the same reason one level
        # up: a packaged release build has no use for building Seabass's
        # own ~90 test binaries either.
        self.subinfo.options.configure.args += ["-DSEABASS_TESTS=OFF", "-DSEABASS_LIBDJINTEROP_TESTS=OFF"]

    def createPackage(self):
        # Keep the two real entry points; drop whatever else CMake put
        # in bin/ (test binaries, etc.) out of the packaged installer.
        self.addExecutableFilter(r"(bin|libexec)/(?!(seabass|seabass-cli)).*")
        if CraftCore.compiler.isMacOS:
            self.blacklist_file.append(self.blueprintDir() / "blacklist_mac.txt")
        return CMakePackageBase.createPackage(self)
