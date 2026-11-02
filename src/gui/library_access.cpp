// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "library_access.hpp"

namespace seabass::gui
{

LibraryAccess::LibraryAccess(QObject *parent)
    : QObject(parent)
{
}

QString LibraryAccess::deniedHeadline()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macOS may not be allowing Seabass to read USB sticks.");
#else
    return QStringLiteral("The system refused to let Seabass read the library on this stick.");
#endif
}

QString LibraryAccess::deniedAdvice()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral(
        "Open System Settings, Privacy & Security, Files & Folders, and switch on Removable Volumes for "
        "Seabass. Then quit Seabass and open it again.");
#else
    return QStringLiteral("Check that your user account is allowed to read the files on this stick.");
#endif
}

QString LibraryAccess::deniedFolderMessage()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral(
        "macOS may not be allowing Seabass to read that folder. Open System Settings, Privacy & Security, "
        "Files & Folders, and allow Seabass the place it is in: Removable Volumes for a USB stick, or "
        "Documents, Desktop or Downloads. Then quit Seabass and open it again.");
#else
    return QStringLiteral(
        "The system refused to let Seabass read the library in that folder. Check that your user account is "
        "allowed to read its files.");
#endif
}

QString LibraryAccess::privacySettingsUrl()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_FilesAndFolders");
#else
    return {};
#endif
}

}  // namespace seabass::gui
