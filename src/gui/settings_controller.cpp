// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "settings_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>

#include <QVariantMap>

#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/rekordbox/rekordbox_settings_fields.hpp"
#include "infrastructure/rekordbox/rekordbox_settings_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_settings_writer.hpp"
#include "gui/edit/changes/device_setting_change.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

// Decodes every recognized settings file on the stick into `groups`.
// *errorMessage is set (and groups left empty) on failure, including the
// "nothing recognized" case -- mirrors the previous synchronous load()'s
// exact wording.
QVariantList buildGroups(const QString &pioneerRoot, QString *errorMessage)
{
    QVariantList groups;
    try {
        auto files = infrastructure::rekordbox::readDeviceSettings(pioneerRoot.toStdString());
        for (const auto &file : files) {
            QVariantMap group;
            group["title"] = QString::fromStdString(file.title);
            group["fileName"] = QString::fromStdString(file.fileName);

            QVariantList fields;
            for (const auto &[label, value] : file.fields) {
                QVariantMap fieldMap;
                fieldMap["label"] = QString::fromStdString(label);
                fieldMap["value"] = QString::fromStdString(value);
                fieldMap["pendingValue"] = QString();
                fieldMap["unsaved"] = false;

                QVariantList options;
                for (const auto &field : infrastructure::rekordbox::allSettingsFields()) {
                    if (field.fileName == file.fileName && field.label == label) {
                        for (const auto &option : field.options) {
                            options << QString::fromStdString(option.name);
                        }
                        break;
                    }
                }
                fieldMap["options"] = options;

                fields << fieldMap;
            }
            group["fields"] = fields;

            groups << group;
        }
        if (groups.isEmpty()) {
            *errorMessage = "No recognized settings files found on this stick.";
        }
    } catch (const std::exception &e) {
        *errorMessage = QString::fromStdString(e.what());
    }
    return groups;
}

// Runs entirely on a background thread (see SettingsController::load()) --
// no access to the controller itself.
SettingsTaskResult runLoadTask(QString pioneerRoot)
{
    SettingsTaskResult result;
    result.groups = buildGroups(pioneerRoot, &result.errorMessage);
    return result;
}

}  // namespace

SettingsController::SettingsController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<SettingsTaskResult>::finished, this, &SettingsController::onTaskFinished);
}

QString SettingsController::changeIdFor(const QString &fileName, const QString &fieldLabel)
{
    return "settings:" + fileName + ":" + fieldLabel;
}

void SettingsController::attachSession()
{
    auto *registry = EditSessionRegistry::instance();
    QString libraryId = registry->libraryIdForPath(m_pioneerRoot);
    LibraryEditSession *session = registry->sessionFor(libraryId);
    if (session == m_session) {
        return;
    }
    if (m_session) {
        disconnect(m_session, nullptr, this, nullptr);
    }
    m_session = session;
    if (!m_session) {
        return;
    }
    m_session->setLibraryPaths(m_pioneerRoot, QString());
    connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            if (changeIdFor(it->first.first, it->first.second) == changeId) {
                m_pending.erase(it);
                break;
            }
        }
    });
    connect(m_session, &LibraryEditSession::saveFinished, this, [this](const QVariantMap &) {
        // Whatever landed is on the stick now: re-decode so `value` shows
        // it and the staged overlay only covers what is still pending.
        load(m_pioneerRoot);
    });
    connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
        m_pending.clear();
        rebuildGroupsView();
    });
}

void SettingsController::load(const QString &pioneerRoot)
{
    if (m_busy) {
        return;
    }
    m_pioneerRoot = pioneerRoot;
    attachSession();
    setErrorMessage({});
    setBusy(true);
    m_watcher.setFuture(QtConcurrent::run(runLoadTask, pioneerRoot));
}

void SettingsController::setField(const QString &fileName, const QString &fieldLabel, const QString &optionName)
{
    setErrorMessage({});
    setStatusMessage({});
    if (!m_session) {
        attachSession();
        if (!m_session) {
            setErrorMessage("This stick's library could not be identified; nothing was changed.");
            return;
        }
    }

    QString oldValue;
    bool known = false;
    for (const QVariant &groupVariant : m_groups) {
        QVariantMap group = groupVariant.toMap();
        if (group["fileName"].toString() != fileName) {
            continue;
        }
        for (const QVariant &fieldVariant : group["fields"].toList()) {
            QVariantMap field = fieldVariant.toMap();
            if (field["label"].toString() == fieldLabel) {
                oldValue = field["value"].toString();
                known = field["options"].toStringList().contains(optionName);
            }
        }
    }
    if (!known) {
        setErrorMessage("\"" + fieldLabel + "\" cannot be set to " + optionName + " -- not a value Seabass knows.");
        return;
    }
    if (optionName == oldValue) {
        unstageField(fileName, fieldLabel);
        return;
    }

    auto change = std::make_unique<DeviceSettingChange>(m_pioneerRoot, fileName, fieldLabel, oldValue, optionName);
    if (!m_session->stage(std::move(change))) {
        return;  // the session reported the lock refusal; the page shows it
    }
    m_pending[{fileName, fieldLabel}] = optionName;
    rebuildGroupsView();
}

void SettingsController::unstageField(const QString &fileName, const QString &fieldLabel)
{
    if (m_pending.erase({fileName, fieldLabel}) == 0) {
        return;
    }
    if (m_session) {
        m_session->unstage(changeIdFor(fileName, fieldLabel));
    }
    rebuildGroupsView();
}

void SettingsController::rebuildGroupsView()
{
    QVariantList view;
    for (const QVariant &groupVariant : m_groups) {
        QVariantMap group = groupVariant.toMap();
        QVariantList fields;
        for (const QVariant &fieldVariant : group["fields"].toList()) {
            QVariantMap field = fieldVariant.toMap();
            auto it = m_pending.find({group["fileName"].toString(), field["label"].toString()});
            if (it != m_pending.end()) {
                field["pendingValue"] = it->second;
                field["unsaved"] = true;
            }
            fields << field;
        }
        group["fields"] = fields;
        view << group;
    }
    m_groupsView = view;
    emit groupsChanged();
}

void SettingsController::onTaskFinished()
{
    SettingsTaskResult result = m_watcher.result();
    m_groups = result.groups;
    rebuildGroupsView();
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
    }
    setBusy(false);
}

void SettingsController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void SettingsController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void SettingsController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
