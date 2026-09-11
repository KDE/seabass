// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>

namespace seabass::application
{

// Who a stick *is*, as opposed to where it currently sits (devicePath and
// mountPoint are reassigned on every replug). Two uses:
//
//  - libraryId(): the key for a library's edit-lock cookie and for "is this
//    the same library" questions. Same rule as StickHardwareInfo::
//    stickIdentifier (filesystem UUID, else label + capacity), so the
//    backup archive's manifest identifier and the lock cookie agree.
//  - isSameStick(): "the exact same physical stick is back", for the
//    stick-removed-while-editing dialog. A content fingerprint is
//    deliberately NOT part of this: the point is to notice a *different*
//    stick that happens to carry the same library.
//
// Every field is best-effort and may be empty; strength() says how much
// the answer can be trusted, so the UI can say "verified by serial number"
// vs "matched by label and size only".
struct StickIdentity
{
    std::string hardwareSerial;   // USB device serial (udev ID_SERIAL_SHORT, STORAGE_DEVICE_DESCRIPTOR); "" if unknown
    std::string filesystemUuid;   // ID_FS_UUID / Windows volume serial as 8 hex digits; "" if none
    std::string label;
    // Set instead of every hardware field when the library is an ordinary
    // directory the user opened rather than a stick that was detected
    // (see MediaController::openFolder). Its absolute path is the only
    // stable identity such a library has, so the caller derives an id
    // from that and puts it here; libraryId() then returns it verbatim.
    // Deliberately not stuffed into filesystemUuid: that would report an
    // identity strength this library has not earned.
    std::string explicitLibraryId;
    std::uint64_t capacityBytes = 0;

    enum class Strength {
        Hardware,    // a hardware serial is known
        Filesystem,  // only the filesystem UUID is known
        Weak,        // label + capacity only
        Folder,      // an ordinary directory, identified by its path
        None,        // nothing at all (no label either)
    };

    Strength strength() const
    {
        // Checked before the hardware fields because a folder library has
        // none of them, and because "which directory" is the whole of its
        // identity -- there is no physical thing to be more sure about.
        if (!explicitLibraryId.empty()) {
            return Strength::Folder;
        }
        if (!hardwareSerial.empty()) {
            return Strength::Hardware;
        }
        if (!filesystemUuid.empty()) {
            return Strength::Filesystem;
        }
        if (!label.empty()) {
            return Strength::Weak;
        }
        return Strength::None;
    }

    static const char *strengthName(Strength strength)
    {
        switch (strength) {
        case Strength::Hardware:
            return "hardware";
        case Strength::Filesystem:
            return "filesystem";
        case Strength::Weak:
            return "weak";
        case Strength::Folder:
            return "folder";
        case Strength::None:
            return "none";
        }
        return "none";
    }

    // Never empty for a stick that has at least a label; empty only when
    // there is genuinely nothing to key on (a blank, unlabelled drive).
    std::string libraryId() const
    {
        if (!explicitLibraryId.empty()) {
            return explicitLibraryId;
        }
        if (!filesystemUuid.empty()) {
            return filesystemUuid;
        }
        if (!label.empty()) {
            return label + "-" + std::to_string(capacityBytes);
        }
        return {};
    }

    // The "exact same stick" rule, strongest evidence first:
    //  1. both sides know a hardware serial: the serials must match, and
    //     if both also know a filesystem UUID those must match too (a
    //     reformatted stick is not "the same stick" for editing purposes:
    //     whatever was on it is gone).
    //  2. otherwise both sides know a filesystem UUID: those must match.
    //  3. otherwise label and capacity must match (weak; the caller should
    //     say so).
    bool isSameStick(const StickIdentity &other) const
    {
        // A folder library is the same library only when it is the same
        // folder. Checked first and on its own: without this, two browsed
        // backups of one stick share a label and a capacity of zero, and
        // the label rule below would call them the same stick.
        if (!explicitLibraryId.empty() || !other.explicitLibraryId.empty()) {
            return explicitLibraryId == other.explicitLibraryId;
        }
        if (!hardwareSerial.empty() && !other.hardwareSerial.empty()) {
            if (hardwareSerial != other.hardwareSerial) {
                return false;
            }
            if (!filesystemUuid.empty() && !other.filesystemUuid.empty()) {
                return filesystemUuid == other.filesystemUuid;
            }
            return true;
        }
        if (!filesystemUuid.empty() && !other.filesystemUuid.empty()) {
            return filesystemUuid == other.filesystemUuid;
        }
        if (label.empty() || other.label.empty()) {
            return false;
        }
        return label == other.label && capacityBytes == other.capacityBytes;
    }

    // A library id as a safe file name: letters, digits, '-', '_' and '.'
    // survive; everything else becomes '_'. Never empty for a non-empty
    // input.
    static std::string sanitizeForFileName(const std::string &id)
    {
        std::string out;
        out.reserve(id.size());
        for (unsigned char c : id) {
            bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
                || c == '_' || c == '.';
            out.push_back(keep ? static_cast<char>(c) : '_');
        }
        if (out == "." || out == "..") {
            out = "_" + out;
        }
        return out;
    }
};

}  // namespace seabass::application
