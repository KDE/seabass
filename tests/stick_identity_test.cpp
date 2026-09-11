// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "application/stick_identity.hpp"

using seabass::application::StickIdentity;
using Strength = StickIdentity::Strength;

namespace
{

StickIdentity make(std::string serial, std::string uuid, std::string label = "MAIN", std::uint64_t capacity = 32000)
{
    StickIdentity id;
    id.hardwareSerial = std::move(serial);
    id.filesystemUuid = std::move(uuid);
    id.label = std::move(label);
    id.capacityBytes = capacity;
    return id;
}

}  // namespace

int main()
{
    // Strength ladder: whatever the strongest known field is.
    {
        assert(make("S1", "U1").strength() == Strength::Hardware);
        assert(make("", "U1").strength() == Strength::Filesystem);
        assert(make("", "").strength() == Strength::Weak);
        assert(make("", "", "").strength() == Strength::None);
        assert(std::string(StickIdentity::strengthName(Strength::Weak)) == "weak");
        std::cout << "case 1 (strength ladder) OK\n";
    }

    // A folder library: explicitLibraryId wins over everything, and says
    // so through its own strength rather than borrowing "filesystem".
    {
        StickIdentity folder;
        folder.label = "restored-backup";
        folder.explicitLibraryId = "folder-0123456789abcdef";
        assert(folder.strength() == Strength::Folder);
        assert(std::string(StickIdentity::strengthName(Strength::Folder)) == "folder");
        assert(folder.libraryId() == "folder-0123456789abcdef");

        // Even with real hardware fields present it still wins: a folder
        // that happens to sit on a removable drive is still identified by
        // which folder it is, not by the drive under it.
        StickIdentity onAStick = make("S1", "U1");
        onAStick.explicitLibraryId = "folder-fedcba9876543210";
        assert(onAStick.libraryId() == "folder-fedcba9876543210");
        assert(onAStick.strength() == Strength::Folder);

        // Two folders are the same stick only when they are the same
        // folder. Both carry the same label and a capacity of zero, which
        // the label rule alone would call a match.
        StickIdentity monday;
        monday.label = "TOURSTICK";
        monday.explicitLibraryId = "folder-aaaaaaaaaaaaaaaa";
        StickIdentity tuesday;
        tuesday.label = "TOURSTICK";
        tuesday.explicitLibraryId = "folder-bbbbbbbbbbbbbbbb";
        assert(!monday.isSameStick(tuesday));
        assert(monday.isSameStick(monday));
        // ...and a folder is never the same stick as real hardware with
        // the same label, in either direction.
        StickIdentity hardware = make("S1", "U1");
        hardware.label = "TOURSTICK";
        assert(!monday.isSameStick(hardware));
        assert(!hardware.isSameStick(monday));
        std::cout << "case 1b (folder identity) OK\n";
    }

    // libraryId: the filesystem UUID when known, else label + capacity
    // (the same shape StickHardwareInfo::stickIdentifier falls back to),
    // else empty. The hardware serial deliberately plays no part: the id
    // names the library (what is on the filesystem), not the hardware.
    {
        assert(make("S1", "1234-ABCD").libraryId() == "1234-ABCD");
        assert(make("S1", "").libraryId() == "MAIN-32000");
        assert(make("", "", "").libraryId().empty());
        std::cout << "case 2 (libraryId fallback ladder) OK\n";
    }

    // Exact-same-stick rule, hardware serial known on both sides.
    {
        assert(make("S1", "U1").isSameStick(make("S1", "U1")));
        assert(!make("S1", "U1").isSameStick(make("S2", "U1")));
        // Same hardware, reformatted meanwhile: not the same stick for
        // editing purposes.
        assert(!make("S1", "U1").isSameStick(make("S1", "U2")));
        // Same hardware, one side has no filesystem UUID (e.g. an
        // unmounted re-detection): the serial alone decides.
        assert(make("S1", "U1").isSameStick(make("S1", "")));
        // Label and capacity are irrelevant once serials are known.
        assert(make("S1", "U1", "A", 1).isSameStick(make("S1", "U1", "B", 2)));
        std::cout << "case 3 (serial rule) OK\n";
    }

    // Filesystem UUID only.
    {
        assert(make("", "U1").isSameStick(make("", "U1")));
        assert(!make("", "U1").isSameStick(make("", "U2")));
        // One side knows a serial, the other does not: fall through to the
        // UUID comparison rather than refusing outright.
        assert(make("S1", "U1").isSameStick(make("", "U1")));
        assert(!make("S1", "U1").isSameStick(make("", "U2")));
        std::cout << "case 4 (filesystem UUID rule) OK\n";
    }

    // Weak: label and capacity, and only when both have a label.
    {
        assert(make("", "").isSameStick(make("", "")));
        assert(!make("", "", "MAIN", 1).isSameStick(make("", "", "MAIN", 2)));
        assert(!make("", "", "MAIN").isSameStick(make("", "", "SPARE")));
        assert(!make("", "", "").isSameStick(make("", "", "")));
        // A UUID on one side and nothing on the other is still weak.
        assert(make("", "U1").isSameStick(make("", "")));
        std::cout << "case 5 (weak rule) OK\n";
    }

    // File-name sanitising keeps the common id shapes readable and never
    // produces a path component that escapes the directory.
    {
        assert(StickIdentity::sanitizeForFileName("1234-ABCD") == "1234-ABCD");
        assert(StickIdentity::sanitizeForFileName("MAIN-32000") == "MAIN-32000");
        assert(StickIdentity::sanitizeForFileName("My Stick/x:y") == "My_Stick_x_y");
        assert(StickIdentity::sanitizeForFileName("..") == "_..");
        assert(StickIdentity::sanitizeForFileName("../../etc") == ".._.._etc");
        std::cout << "case 6 (sanitizeForFileName) OK\n";
    }

    std::cout << "stick_identity_test: all cases passed\n";
    return 0;
}
