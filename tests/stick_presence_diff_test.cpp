// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "application/stick_presence_diff.hpp"

using seabass::application::StickIdentity;
using seabass::application::diffStickPresence;
using seabass::application::findAwaited;
using seabass::application::matchStrength;

namespace
{

StickIdentity make(const std::string &serial, const std::string &uuid, const std::string &label,
                   std::uint64_t capacity = 32ull * 1024 * 1024 * 1024)
{
    StickIdentity id;
    id.hardwareSerial = serial;
    id.filesystemUuid = uuid;
    id.label = label;
    id.capacityBytes = capacity;
    return id;
}

}  // namespace

int main()
{
    const StickIdentity gig = make("SER-A", "1111-AAAA", "GIG");
    const StickIdentity spare = make("SER-B", "2222-BBBB", "SPARE");

    // Pulling one stick: exactly that one is gone, nothing appeared.
    {
        auto diff = diffStickPresence({gig, spare}, {spare});
        assert(diff.gone.size() == 1);
        assert(diff.gone[0].libraryId() == gig.libraryId());
        assert(diff.appeared.empty());
        std::cout << "case 1 (one stick pulled) OK\n";
    }

    // The same stick back on another mount point: it appeared, and it
    // matches what was awaited, with hardware evidence.
    {
        auto diff = diffStickPresence({spare}, {spare, gig});
        assert(diff.gone.empty());
        assert(diff.appeared.size() == 1);
        auto awaited = findAwaited({gig}, diff.appeared[0]);
        assert(awaited.has_value());
        assert(awaited->libraryId() == gig.libraryId());
        assert(matchStrength(*awaited, diff.appeared[0]) == StickIdentity::Strength::Hardware);
        std::cout << "case 2 (same stick returned) OK\n";
    }

    // A different stick with the same label and size is not the one
    // being awaited when serials are known.
    {
        const StickIdentity impostor = make("SER-C", "3333-CCCC", "GIG");
        auto diff = diffStickPresence({spare}, {spare, impostor});
        assert(diff.appeared.size() == 1);
        assert(!findAwaited({gig}, diff.appeared[0]).has_value());
        std::cout << "case 3 (same label, different stick) OK\n";
    }

    // The same hardware, reformatted (new filesystem UUID): not the same
    // stick for editing purposes, whatever was on it is gone.
    {
        const StickIdentity reformatted = make("SER-A", "9999-ZZZZ", "GIG");
        assert(!findAwaited({gig}, reformatted).has_value());
        std::cout << "case 4 (reformatted stick is not the same stick) OK\n";
    }

    // Without serials or UUIDs, label and size are all there is: the
    // match is reported as weak.
    {
        const StickIdentity weakBefore = make("", "", "GIG");
        const StickIdentity weakAfter = make("", "", "GIG");
        auto awaited = findAwaited({weakBefore}, weakAfter);
        assert(awaited.has_value());
        assert(matchStrength(*awaited, weakAfter) == StickIdentity::Strength::Weak);
        assert(!findAwaited({weakBefore}, make("", "", "GIG", 1)).has_value());
        std::cout << "case 5 (label+size only: weak match) OK\n";
    }

    // No change: nothing reported either way.
    {
        auto diff = diffStickPresence({gig, spare}, {spare, gig});
        assert(diff.gone.empty() && diff.appeared.empty());
        std::cout << "case 6 (no change) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
