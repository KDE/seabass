# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: BSD-2-Clause

import os

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
        # SQLCipher 4, because rekordbox writes that format; Craft's own
        # libs/sqlcipher is 3.4.2, which cannot read it (see
        # craft-blueprint/libs/sqlcipher4).
        self.runtimeDependencies["libs/sqlcipher4"] = None
        # Optional: Seabass's own CMakeLists.txt falls back to
        # application::NullTrackMetadataProbe when TagLib isn't found.
        self.runtimeDependencies["libs/taglib"] = None
        if CraftCore.compiler.isLinux:
            # The AppImage cannot use the system's Plasma integration, so it
            # carries KDE's desktop style itself -- the look the app has on a
            # Plasma desktop. The app selects it when APPIMAGE is set (see
            # src/gui/controls_style.cpp). Same set NeoChat, Tokodon and
            # Marknote bundle.
            self.runtimeDependencies["kde/frameworks/tier3/qqc2-desktop-style"] = None
            self.runtimeDependencies["kde/plasma/breeze"] = None
            self.runtimeDependencies["kde/frameworks/tier1/breeze-icons"] = None


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
        # The channel the app reports and checks for updates on, which
        # CMake defaults to "dev". A release tag names it,
        # releases/<channel>/X.Y.Z, and CI hands the tag to Craft as
        # CI_COMMIT_TAG; a package built by hand takes it from
        # SEABASS_RELEASE_CHANNEL, which is how the universal .dmg is made
        # until the macOS job signs on Invent (docs/releasing.md). Anything
        # else stays "dev": a wrong channel would send the app to the wrong
        # place for updates, and dev is the one that never looks.
        channel = os.environ.get("SEABASS_RELEASE_CHANNEL", "")
        tag = os.environ.get("CI_COMMIT_TAG", "")
        if not channel and tag.startswith("releases/"):
            channel = tag.split("/")[1]
        if channel in ("alpha", "beta", "stable"):
            self.subinfo.options.configure.args += [f"-DSEABASS_RELEASE_CHANNEL={channel}"]

    def createPackage(self):
        # Keep the two real entry points; drop whatever else CMake put
        # in bin/ (test binaries, etc.) out of the packaged installer.
        self.addExecutableFilter(r"(bin|libexec)/(?!(seabass|seabass-cli)).*")
        if CraftCore.compiler.isMacOS:
            self.blacklist_file.append(self.blueprintDir() / "blacklist_mac.txt")
        if CraftCore.compiler.isWindows:
            # The Start menu entries. Craft's NSIS installer makes none
            # unless it is told which executable to point at, and without
            # one the app could only be found by digging bin\seabass.exe
            # out of the install folder (Bart, testing the 0.8 installer).
            # The icon is the one built into the .exe
            # (src/gui/win/app_icon.rc). The uninstaller sits beside it,
            # as it does for most Windows apps; the template's uninstall
            # removes the whole Start menu folder.
            self.defines["shortcuts"] = [
                {"name": "Seabass", "target": "bin/seabass.exe", "description": self.subinfo.description},
                {"name": "Uninstall Seabass", "target": "uninstall.exe"},
            ]
            # A desktop shortcut too, as a choice on the installer's
            # components page: ticked, so a plain Next-Next-Install gets
            # one, and untickable for whoever does not want it. Same
            # sections as KDE Connect's and LabPlot's installers use.
            self.defines["sections"] = r"""
                Section "Desktop shortcut"
                    CreateShortCut "$DESKTOP\Seabass.lnk" "$INSTDIR\bin\seabass.exe"
                SectionEnd
                """
            # The template's uninstall removes the program, its Start menu
            # folder and its registry entries; these remove what Seabass
            # itself leaves behind, so an uninstall is a clean one. Qt's
            # caches always go. The app's own state -- its settings,
            # QSettings("seabass", "seabass") in HKCU\Software\seabass, and
            # Qt's AppDataLocation for that name (stick history, edit
            # locks) -- goes too, except when the uninstall is silent: that
            # is how the installer clears out the previous version before
            # an upgrade (the template's ExecWait ... /S), and an upgrade
            # keeps the user's settings.
            #
            # ~\Seabass is never touched: it is the user's own, their
            # library backups and metadata, in a folder with the app's name
            # on it (infrastructure/paths/seabass_paths.cpp's localRoot()).
            #
            # $DESKTOP and $LOCALAPPDATA follow the install mode: an
            # all-users install made its desktop shortcut on the public
            # desktop, but Seabass keeps its state per user, under the
            # user's own $LOCALAPPDATA -- hence SetShellVarContext current
            # after the shortcut is gone.
            self.defines["un_sections"] = r"""
                Section "Un.Remove desktop shortcut"
                    Delete "$DESKTOP\Seabass.lnk"
                SectionEnd
                Section "Un.Remove Seabass's own files"
                    SetShellVarContext current
                    RMDir /r "$LOCALAPPDATA\seabass\cache"
                    ${IfNot} ${Silent}
                        RMDir /r "$LOCALAPPDATA\seabass"
                        DeleteRegKey HKCU "Software\seabass"
                    ${EndIf}
                    ; Gone if the cache was all there was in it.
                    RMDir "$LOCALAPPDATA\seabass"
                SectionEnd
                """
        return CMakePackageBase.createPackage(self)
