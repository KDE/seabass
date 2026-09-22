# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: BSD-2-Clause

# SQLCipher 4, for reading rekordbox's exportLibrary.db (OneLibrary).
#
# craft-blueprints-kde's libs/sqlcipher is 3.4.2, which cannot open that
# database at all ("file is not a database": rekordbox writes the
# SQLCipher 4 format), and other applications' existing databases depend
# on it staying at 3.x. This builds 4.x alongside for Seabass, the way
# Homebrew and Debian package it: the library is renamed from libsqlite3
# to libsqlcipher so it cannot be mistaken for, or collide with, plain
# SQLite. Seabass loads it at runtime (sqlcipher_dyn.cpp); nothing links
# against it.
#
# Unix builds with autosetup; MSVC with Makefile.msc, producing
# libsqlcipher.dll (sqlcipher_dyn.cpp tries that name after MSYS2's
# libsqlcipher-0.dll). MinGW is not handled.

import glob
import os
from pathlib import Path

import info
import utils
from CraftCore import CraftCore
from Package.AutoToolsPackageBase import AutoToolsPackageBase
from Package.MSBuildPackageBase import MSBuildPackageBase
from Utils.Arguments import Arguments
from Utils import CraftHash


class subinfo(info.infoclass):
    def setTargets(self):
        self.description = "SQLite extension providing 256-bit AES encryption, version 4"
        self.webpage = "https://www.zetetic.net/sqlcipher/"
        for ver in ["4.19.0"]:
            self.targets[ver] = f"https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v{ver}.tar.gz"
            self.archiveNames[ver] = f"sqlcipher-{ver}.tar.gz"
            self.targetInstSrc[ver] = f"sqlcipher-{ver}"
        self.targetDigests["4.19.0"] = (["7075f96cbabe45b4ecfc2e6b1745a625f856f695b0827a5506ce9ed85b906aa0"], CraftHash.HashAlgorithm.SHA256)
        self.defaultTarget = "4.19.0"

    def setDependencies(self):
        self.runtimeDependencies["virtual/base"] = None
        self.runtimeDependencies["libs/openssl"] = None
        self.runtimeDependencies["libs/zlib"] = None
        # The Unix build generates sources with tclsh; nothing uses Tcl at
        # runtime. Makefile.msc builds its own jimsh for that instead.
        if not CraftCore.compiler.isMSVC():
            self.buildDependencies["libs/tcl"] = None


# Same feature set on every platform; SQLITE_EXTRA_INIT/SHUTDOWN are what
# SQLCipher 4.7+ needs to register its codec.
FEATURE_DEFINES = [
    "-DSQLITE_HAS_CODEC",
    "-DSQLITE_ENABLE_JSON1",
    "-DSQLITE_ENABLE_FTS3",
    "-DSQLITE_ENABLE_FTS3_PARENTHESIS",
    "-DSQLITE_ENABLE_FTS5",
    "-DSQLITE_ENABLE_COLUMN_METADATA",
    "-DSQLITE_EXTRA_INIT=sqlcipher_extra_init",
    "-DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown",
]


class PackageAutotools(AutoToolsPackageBase):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        opts = self.subinfo.options
        # autosetup, not autoconf: it rejects --cache-file and has no
        # configure.ac to regenerate.
        opts.configure.autoreconf = False
        opts.configure.noCacheFile = True
        opts.configure.staticArgs = ["--disable-static"]
        opts.configure.args += [
            # Leaves out the Tcl extension and tests; code generation
            # still needs a tclsh, passed to make below.
            "--disable-tcl",
            "--with-tempstore=yes",
            "--dll-basename=libsqlcipher",
        ]
        # The same set Homebrew uses.
        opts.configure.cflags += " " + " ".join(FEATURE_DEFINES)
        opts.configure.ldflags += " -lcrypto"
        # --disable-tcl leaves the Makefile's TCLSH_CMD at "false", which
        # the code generation steps still call.
        opts.make.args += [f"TCLSH_CMD={self._tclsh()}"]
        # -j1: the install step races on mkdir (Homebrew serialises it too);
        # the last -j on make's command line wins.
        opts.install.args += [f"TCLSH_CMD={self._tclsh()}", "-j1"]
        # Building x86_64 on an arm64 Mac is a cross build as far as Craft is
        # concerned (it reads the kernel, so even Rosetta counts), and for
        # those it passes --host, --build and --target. autosetup takes the
        # first two and stops at the third: "Unknown option --target".
        # Dropping it loses nothing -- there is no separate target machine
        # here, and the architecture reaches the compiler through the -arch
        # Craft appends to CC and CXX.
        if CraftCore.compiler.isMacOS and not CraftCore.compiler.isNative():
            self.platform = Arguments([arg for arg in self.platform.get() if not str(arg).startswith("--target=")])

    def _tclsh(self):
        return str(CraftCore.standardDirs.craftRoot() / "bin" / "tclsh8.6")

    def configure(self):
        # autosetup runs itself with the first "tclsh" on PATH before it
        # falls back to its bundled jimsh; on macOS that is the system's
        # Tcl 8.5, which it cannot run under ("invalid command name
        # tailcall").
        with utils.ScopedEnv({"autosetup_tclsh": self._tclsh()}):
            return super().configure()

    def postInstall(self):
        image = Path(self.installDir())
        # libsqlite3.* -> libsqlcipher.*, symlinks re-pointed; plain
        # SQLite's own names stay free for libs/sqlite.
        for path in sorted(glob.glob(str(image / "lib" / "libsqlite3*"))):
            path = Path(path)
            renamed = path.with_name(path.name.replace("sqlite3", "sqlcipher"))
            if path.is_symlink():
                target = os.readlink(path).replace("sqlite3", "sqlcipher")
                path.unlink()
                if renamed.exists() or renamed.is_symlink():
                    renamed.unlink()
                renamed.symlink_to(target)
            else:
                if not utils.moveFile(path, renamed):
                    return False
        libsqlcipher = image / "lib" / "libsqlcipher.dylib"
        if libsqlcipher.is_symlink():
            target = os.readlink(libsqlcipher).replace("sqlite3", "sqlcipher")
            libsqlcipher.unlink()
            libsqlcipher.symlink_to(target)
        pc = image / "lib" / "pkgconfig" / "sqlite3.pc"
        if pc.exists():
            content = pc.read_text(encoding="utf-8").replace("-lsqlite3", "-lsqlcipher")
            (pc.parent / "sqlcipher.pc").write_text(content, encoding="utf-8")
            pc.unlink()
        # Headers under include/sqlcipher, as upstream and the distros do;
        # the shell and its man page would collide with plain SQLite's.
        include = image / "include"
        (include / "sqlcipher").mkdir(parents=True, exist_ok=True)
        for header in ["sqlite3.h", "sqlite3ext.h"]:
            if (include / header).exists():
                if not utils.moveFile(include / header, include / "sqlcipher" / header):
                    return False
        for path in [image / "bin" / "sqlite3", image / "share" / "man" / "man1" / "sqlite3.1"]:
            if path.exists():
                path.unlink()
        return super().postInstall()


class PackageMSVC(MSBuildPackageBase):
    """SQLCipher's documented MSVC build: `nmake /f Makefile.msc dll`,
    OpenSSL from Craft, the DLL named so it cannot be taken for SQLite's."""

    def configure(self):
        return True

    def _nmakeArgs(self):
        craftRoot = CraftCore.standardDirs.craftRoot()
        defines = FEATURE_DEFINES + ["-DSQLITE_TEMP_STORE=2", f"-I{craftRoot / 'include'}"]
        return [
            "nmake",
            "/f",
            "Makefile.msc",
            "dll",
            "USE_AMALGAMATION=1",
            "NO_TCL=1",
            # The CRT the rest of Craft links, and the one KMyMoney's
            # libs/sqlcipher needed to stop crashing.
            "USE_CRT_DLL=1",
            "SQLITE3DLL=libsqlcipher.dll",
            "SQLITE3LIB=libsqlcipher.lib",
            "SQLITE3EXE=sqlcipher.exe",
            f"OPTS={' '.join(defines)}",
            # Command-line macros replace the makefile's own, so rpcrt4.lib
            # (which it adds) is repeated here.
            f"LTLIBPATHS=/LIBPATH:{craftRoot / 'lib'}",
            "LTLIBS=rpcrt4.lib libcrypto.lib",
            # Makefile.msc line 1245 sets, unconditionally and without
            # consulting USE_CRT_DLL:
            #
            #     LDFLAGS = /NODEFAULTLIB:msvcrt /DEBUG $(LDOPTS)
            #
            # That is correct for its own default, the STATIC CRT, where
            # objects carry /DEFAULTLIB:libcmt and msvcrt must be kept
            # out so the two are not mixed. We ask for USE_CRT_DLL=1
            # above, which makes the makefile compile everything -MD, so
            # every object carries /DEFAULTLIB:msvcrt -- and then this
            # excludes exactly the library they asked for. The two
            # settings contradict each other and the makefile has no
            # guard against it.
            #
            # It shows up first on lemon.exe, SQLite's parser generator,
            # because that links early and with $(LDFLAGS) directly:
            #
            #     lemon.obj : error LNK2019: unresolved external symbol
            #                 strlen ... mainCRTStartup ... __imp_fclose
            #     lemon.exe : fatal error LNK1120: 43 unresolved externals
            #
            # It is not lemon-specific: $(LDFLAGS) appears on 45 link
            # lines in that makefile, the DLL target among them, so the
            # same contradiction applies to everything built here.
            #
            # A command-line macro beats a makefile definition in nmake's
            # precedence, so setting it here wins. /DEBUG is kept because
            # that is what the makefile's own SYMBOLS branch was adding;
            # only the self-contradictory exclusion is dropped. $(LDOPTS)
            # is empty unless someone sets it, and nothing here does.
            "LDFLAGS=/DEBUG",
        ]

    def make(self):
        self.enterSourceDir()
        # Print the environment nmake is about to inherit, before running
        # it. This is a diagnostic, not a fix, and it is here rather than
        # in .gitlab-ci.yml because this is the only place that sees what
        # nmake actually sees: the CI job's own shell is several layers
        # and one vcvarsall import away.
        #
        # Why these five. The MSVC build currently dies linking lemon.exe,
        # SQLite's parser generator, with 43 unresolved externals
        # including strlen and mainCRTStartup -- a linker that cannot find
        # the C runtime at all. Two candidates survive, and each of these
        # names decides between them:
        #
        #   LIB      lemon links with no explicit library path, so the CRT
        #            comes entirely from this and from the /DEFAULTLIB
        #            directives embedded in lemon.obj. Craft imports the
        #            full vcvarsall environment, so this is normally
        #            right, which is exactly why it is worth reading
        #            rather than assuming.
        #   LDFLAGS  nmake takes environment variables as macros, and
        #            $(LDFLAGS) is on lemon's link line verbatim. Craft
        #            sets it for MSVC builds. Anything Unix-shaped in
        #            there, or any /NODEFAULTLIB, lands straight in that
        #            link.
        #   CL, _LINK_  the implicit-option variables cl and link read on
        #            their own; either can inject a flag nothing in this
        #            file or that makefile mentions.
        #
        # Ruled out already, so nobody re-treads them: LTLIBS (lemon does
        # not use it -- it is $(NLTLINKOPTS)/$(NLTLIBPATHS) on that line)
        # and the NLT* macros themselves (both empty, since
        # USE_NATIVE_LIBPATHS defaults to 0 in Makefile.msc).
        #
        # print() rather than CraftCore.log: nothing else in this
        # repository's blueprints uses Craft's logger, so its exact
        # surface is unverified here, and an AttributeError would turn a
        # link failure into a blueprint crash and lose the very output
        # this exists to capture. stdout is captured by the job either
        # way.
        if CraftCore.compiler.isMSVC():
            for name in ("LIB", "INCLUDE", "LDFLAGS", "CL", "_LINK_"):
                print(f"sqlcipher4-env: {name}={os.environ.get(name, '<unset>')}", flush=True)
        return utils.system(self._nmakeArgs(), cwd=self.sourceDir())

    def install(self):
        self.cleanImage()
        src = Path(self.sourceDir())
        image = Path(self.installDir())
        for sub in ["bin", "lib", "include/sqlcipher"]:
            (image / sub).mkdir(parents=True, exist_ok=True)
        copies = [
            (src / "libsqlcipher.dll", image / "bin" / "libsqlcipher.dll"),
            (src / "libsqlcipher.lib", image / "lib" / "libsqlcipher.lib"),
            (src / "sqlite3.h", image / "include" / "sqlcipher" / "sqlite3.h"),
            (src / "sqlite3ext.h", image / "include" / "sqlcipher" / "sqlite3ext.h"),
        ]
        for source, dest in copies:
            if not utils.copyFile(source, dest, linkOnly=False):
                return False
        pdb = src / "libsqlcipher.pdb"
        if pdb.exists() and not utils.copyFile(pdb, image / "bin" / "libsqlcipher.pdb", linkOnly=False):
            return False
        return True


if CraftCore.compiler.isMSVC():

    class Package(PackageMSVC):
        pass

else:

    class Package(PackageAutotools):
        pass
