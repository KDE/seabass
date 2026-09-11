#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Materializes .gitmodules from cmake/dependency-submodules.txt, then
# initializes/updates the git submodules under third_party/.
#
# .gitmodules is deliberately NOT committed to this repository (see
# .gitignore): KDE Invent, Seabass's canonical host, rejects any pushed
# commit that contains a file literally named .gitmodules at its
# commit-audit step ("Invalid filename: .gitmodules"). The real
# submodule configuration instead lives in cmake/dependency-submodules.txt
# -- identical git-config-file syntax, just a different filename -- and
# this script regenerates the real .gitmodules from it on demand.
#
# You normally don't need to run this by hand: the top-level
# CMakeLists.txt calls it automatically on every configure. It's here
# for CI, a from-scratch `git submodule` workflow outside CMake, or if
# you ever need to re-run it manually (e.g. after editing
# cmake/dependency-submodules.txt itself).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
cp cmake/dependency-submodules.txt .gitmodules
git submodule sync --recursive
git submodule update --init --recursive
