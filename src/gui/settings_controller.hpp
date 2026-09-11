// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantList>

#include <map>
#include <utility>

namespace seabass::gui
{

class LibraryEditSession;

// Result of a background load -- see SettingsController::load(). Built
// entirely on a worker thread, with no access to the controller.
struct SettingsTaskResult
{
    QVariantList groups;
    QString errorMessage;  // empty on success
};

// Wraps readDeviceSettings() for QML: decodes whichever of rekordbox's
// player/mixer settings files are present on a stick, exposed as
// `groups` -- a list of { title, fileName, fields: [{label, value,
// options, pendingValue, unsaved}] }.
//
// Edits are staged, not written: setField() adds one PendingChange per
// field to the library's LibraryEditSession (the first one takes the
// edit lock), `groups` shows the staged value with `unsaved` set, and
// the page's Save writes them all. Loading is disk I/O and runs on a
// background thread as before.
class SettingsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList groups READ groups NOTIFY groupsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY groupsChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    explicit SettingsController(QObject *parent = nullptr);

    QVariantList groups() const { return m_groupsView; }
    bool busy() const { return m_busy; }
    int pendingCount() const { return static_cast<int>(m_pending.size()); }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }

    // pioneerRoot is the DetectedStick.rekordboxPath.
    Q_INVOKABLE void load(const QString &pioneerRoot);

    // Stages one field's new value (see the class comment). Refuses (sets
    // errorMessage, stages nothing) for any field/option Seabass doesn't
    // fully recognize; choosing the value already on the stick unstages.
    Q_INVOKABLE void setField(const QString &fileName, const QString &fieldLabel, const QString &optionName);
    Q_INVOKABLE void unstageField(const QString &fileName, const QString &fieldLabel);

signals:
    void groupsChanged();
    void busyChanged();
    void errorMessageChanged();
    void statusMessageChanged();

private:
    void onTaskFinished();
    void setBusy(bool busy);
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);
    void attachSession();
    void rebuildGroupsView();
    static QString changeIdFor(const QString &fileName, const QString &fieldLabel);

    QFutureWatcher<SettingsTaskResult> m_watcher;
    QString m_pioneerRoot;
    QVariantList m_groups;      // as decoded from the stick
    QVariantList m_groupsView;  // m_groups with the staged values overlaid
    std::map<std::pair<QString, QString>, QString> m_pending;  // (fileName, label) -> staged option
    QPointer<LibraryEditSession> m_session;
    bool m_busy = false;
    QString m_errorMessage;
    QString m_statusMessage;
};

}  // namespace seabass::gui
