// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>

namespace seabass::gui
{

// What to tell someone whose library the system will not let Seabass
// open (infrastructure::ReadAccess::Denied), in this platform's words.
// On macOS that is almost always the privacy setting for removable
// volumes, which a missed or refused prompt leaves off and a local
// build asks for again; elsewhere Seabass does not know why, and says
// only what it saw.
class LibraryAccess : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString deniedHeadline READ deniedHeadline CONSTANT)
    Q_PROPERTY(QString deniedAdvice READ deniedAdvice CONSTANT)
    // Where the setting is, for Qt.openUrlExternally; "" where there is
    // no one place to send someone.
    Q_PROPERTY(QString privacySettingsUrl READ privacySettingsUrl CONSTANT)

public:
    explicit LibraryAccess(QObject *parent = nullptr);

    static QString deniedHeadline();
    static QString deniedAdvice();
    // The same for a folder opened with Open Folder, which need not be on
    // a stick at all: one sentence, what was refused and where to look.
    static QString deniedFolderMessage();
    // macOS: System Settings at Privacy & Security, Files & Folders.
    static QString privacySettingsUrl();
};

}  // namespace seabass::gui
