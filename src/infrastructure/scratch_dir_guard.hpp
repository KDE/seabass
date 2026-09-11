// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <system_error>
#include <utility>

namespace seabass::infrastructure
{

// RAII cleanup for a scratch directory holding a temporary local copy of
// a file that's slow to access on its real (often removable) media --
// see e.g. EngineLibraryCreator::create() and sync_controller.cpp's own
// whole-file-replace path for why this pattern exists.
//
// Move-only, deliberately: a user-declared destructor suppresses the
// implicit move constructor, so a copyable version of this type is a
// real correctness hazard, not just style -- if it were copyable,
// std::optional<ScratchDirGuard>::emplace(ScratchDirGuard{path}) would
// silently fall back to COPYING the temporary argument into place, and
// that temporary's destructor (running at the end of the full
// expression, i.e. almost immediately) would delete the real scratch
// directory within microseconds of creating it -- before whatever just
// copied a file into it ever gets to use it. This was confirmed as the
// actual root cause of a real, reproducible sync failure ("exportLibrary
// .db does not exist" on every attempt) once three separate hand-rolled
// copies of this same struct existed and one of them was constructed via
// exactly that emplace-with-a-temporary pattern. Keep this move-only.
class ScratchDirGuard
{
public:
    explicit ScratchDirGuard(std::filesystem::path path) : path(std::move(path)) {}
    ScratchDirGuard(const ScratchDirGuard &) = delete;
    ScratchDirGuard &operator=(const ScratchDirGuard &) = delete;
    ScratchDirGuard(ScratchDirGuard &&) = default;
    ScratchDirGuard &operator=(ScratchDirGuard &&) = default;
    ~ScratchDirGuard()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path path;
};

}  // namespace seabass::infrastructure
