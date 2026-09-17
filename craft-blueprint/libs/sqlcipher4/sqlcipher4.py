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
# Unix only for now: Windows builds SQLCipher through Makefile.msc, which
# this blueprint does not drive, and still uses libs/sqlcipher.

import glob
import os
from pathlib import Path

import info
import utils
from CraftCore import CraftCore
from Package.AutoToolsPackageBase import AutoToolsPackageBase
from Utils import CraftHash


class subinfo(info.infoclass):
    def registerOptions(self):
        self.parent.package.categoryInfo.platforms = CraftCore.compiler.Platforms.NotWindows

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
        # The build generates sources with tclsh; nothing uses Tcl at runtime.
        self.buildDependencies["libs/tcl"] = None


class Package(AutoToolsPackageBase):
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
        # The same set Homebrew uses; SQLITE_EXTRA_INIT/SHUTDOWN are what
        # SQLCipher 4.7+ needs to register its codec.
        opts.configure.cflags += (
            " -DSQLITE_HAS_CODEC -DSQLITE_ENABLE_JSON1 -DSQLITE_ENABLE_FTS3"
            " -DSQLITE_ENABLE_FTS3_PARENTHESIS -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_COLUMN_METADATA"
            " -DSQLITE_EXTRA_INIT=sqlcipher_extra_init -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown"
        )
        opts.configure.ldflags += " -lcrypto"
        # --disable-tcl leaves the Makefile's TCLSH_CMD at "false", which
        # the code generation steps still call.
        opts.make.args += [f"TCLSH_CMD={self._tclsh()}"]
        # -j1: the install step races on mkdir (Homebrew serialises it too);
        # the last -j on make's command line wins.
        opts.install.args += [f"TCLSH_CMD={self._tclsh()}", "-j1"]

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
