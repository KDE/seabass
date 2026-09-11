// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "application/stick_identity.hpp"

namespace seabass::application
{

// What changed between two detections of the mounted sticks, by
// identity rather than by mount point (a stick pulled and put back may
// land on another mount point; a different stick may land on the old
// one). Qt-free so the rule is unit-testable; MediaController turns the
// result into signals.
struct StickPresenceDiff
{
    std::vector<StickIdentity> gone;      // in `before`, no same stick in `after`
    std::vector<StickIdentity> appeared;  // in `after`, no same stick in `before`
};

inline bool containsSameStick(const std::vector<StickIdentity> &sticks, const StickIdentity &identity)
{
    return std::any_of(sticks.begin(), sticks.end(),
                       [&](const StickIdentity &other) { return identity.isSameStick(other); });
}

inline StickPresenceDiff diffStickPresence(const std::vector<StickIdentity> &before,
                                           const std::vector<StickIdentity> &after)
{
    StickPresenceDiff diff;
    for (const StickIdentity &identity : before) {
        if (!containsSameStick(after, identity)) {
            diff.gone.push_back(identity);
        }
    }
    for (const StickIdentity &identity : after) {
        if (!containsSameStick(before, identity)) {
            diff.appeared.push_back(identity);
        }
    }
    return diff;
}

// The evidence isSameStick() had for two identities that did match:
// what the removed-stick dialog tells the user ("matched by label and
// size only" for Weak).
inline StickIdentity::Strength matchStrength(const StickIdentity &a, const StickIdentity &b)
{
    if (!a.explicitLibraryId.empty() && !b.explicitLibraryId.empty()) {
        return StickIdentity::Strength::Folder;
    }
    if (!a.hardwareSerial.empty() && !b.hardwareSerial.empty()) {
        return StickIdentity::Strength::Hardware;
    }
    if (!a.filesystemUuid.empty() && !b.filesystemUuid.empty()) {
        return StickIdentity::Strength::Filesystem;
    }
    return StickIdentity::Strength::Weak;
}

// The first entry of `awaited` that is the same stick as `identity`.
inline std::optional<StickIdentity> findAwaited(const std::vector<StickIdentity> &awaited,
                                                const StickIdentity &identity)
{
    auto it = std::find_if(awaited.begin(), awaited.end(),
                           [&](const StickIdentity &other) { return identity.isSameStick(other); });
    if (it == awaited.end()) {
        return std::nullopt;
    }
    return *it;
}

}  // namespace seabass::application
