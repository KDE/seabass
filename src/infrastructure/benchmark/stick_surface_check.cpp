// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/benchmark/stick_surface_check.hpp"

namespace seabass::infrastructure::benchmark
{

domain::StickSurfaceCheck StickSurfaceCheck::run(const std::string &stickRoot, const SurfaceProgress &progress,
                                                 const application::CancellationToken &cancel)
{
    try {
        return storageprobe::SurfaceCheck::run(stickRoot, progress, [cancel] { return cancel.cancelled(); });
    } catch (const storageprobe::Cancelled &) {
        throw application::OperationCancelled();
    }
}

}  // namespace seabass::infrastructure::benchmark
