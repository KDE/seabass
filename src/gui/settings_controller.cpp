// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "settings_controller.hpp"

#include "gui/async_request.hpp"
#include "gui/future_result.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>
#include <map>
#include <optional>
#include <string>

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

// Decodes every recognized settings file on the stick into `groups`,
// one group per category (see settingsCategoryOrder()), in that order.
//
// Grouped by what a setting is about, not by the file it lives in. Every
// field carries its own fileName instead, because that is what staging
// and writing need; a heading reading "Player settings (2)" was the file
// layout leaking onto the page and told nobody what was under it.
//
// *errorMessage is set (and groups left empty) on failure, including the
// "nothing recognized" case.
QVariantList buildGroups(const QString &pioneerRoot, QString *errorMessage)
{
    namespace rb = infrastructure::rekordbox;
    QVariantList groups;
    try {
        std::map<std::string, QVariantList> fieldsByCategory;
        for (const auto &file : rb::readDeviceSettings(pioneerRoot.toStdString())) {
            for (const auto &[label, value] : file.fields) {
                const rb::SettingsFieldDescriptor *descriptor = nullptr;
                for (const auto &field : rb::allSettingsFields()) {
                    if (field.fileName == file.fileName && field.label == label) {
                        descriptor = &field;
                        break;
                    }
                }
                if (!descriptor) {
                    continue;  // the reader only decodes table fields, so this cannot happen
                }

                QVariantMap fieldMap;
                fieldMap["label"] = QString::fromStdString(label);
                fieldMap["fileName"] = QString::fromStdString(file.fileName);
                fieldMap["value"] = QString::fromStdString(value);
                fieldMap["pendingValue"] = QString();
                fieldMap["unsaved"] = false;
                fieldMap["explanation"] = QString::fromStdString(descriptor->explanation);

                QStringList options;
                for (const auto &option : descriptor->options) {
                    options << QString::fromStdString(option.name);
                }
                fieldMap["options"] = options;
                // Exactly off and on, and only while the stick holds one
                // of them -- see isOffOnSwitch(). A lock/unlock pair is a
                // choice between named states and stays a drop-down.
                fieldMap["isSwitch"] = rb::isOffOnSwitch(descriptor->options, value);

                fieldsByCategory[descriptor->category] << fieldMap;
            }
        }
        for (const auto &category : rb::settingsCategoryOrder()) {
            const auto found = fieldsByCategory.find(category);
            if (found == fieldsByCategory.end()) {
                continue;  // a stick without a mixer file has no mixer headings at all
            }
            QVariantMap group;
            group["title"] = QString::fromStdString(category);
            group["fields"] = found->second;
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
    connect(m_session, &LibraryEditSession::stickPresenceChanged, this, [this]() {
        // The stick is back (#114). Pulled mid-save, the read above ran
        // while it was gone and found no settings files, and nothing read
        // it again: the page said the stick had none until it was left and
        // opened again.
        if (!m_session || !m_session->stickPresent()) {
            return;
        }
        // Matched by label and size only: it may be another stick. Keep
        // the page as it was rather than lay a stranger's settings under
        // what is staged, and say why.
        if (!m_session->stickIdentityVerified()) {
            setStatusMessage("A stick with the same label and size is back, but Seabass cannot tell it is the same "
                             "one, so this page still shows what it read before. Open the page again to read the "
                             "stick that is plugged in.");
            return;
        }
        // Back at another mount point: a new library there, not this page's.
        // Nothing staged here can be saved; reading this page's path again
        // would read whatever is mounted there now.
        if (m_session->stickBackElsewhere()) {
            setStatusMessage("The stick was plugged back in at another place. Discard these changes and open it "
                             "again from the stick list.");
            return;
        }
        setStatusMessage({});
        // Read it now, and square what is staged with what it holds once
        // read (reconcileStaged). A save still writing reads it when it
        // finishes; the reconciling waits for that read.
        m_reconcileOnRead = true;
        if (!m_session->writing()) {
            load(m_pioneerRoot);
        }
    });
    connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
        m_pending.clear();
        rebuildGroupsView();
    });
}

void SettingsController::load(const QString &pioneerRoot)
{
    if (m_busy) {
        // A read already running may be the one that found the stick gone:
        // read again once it is done rather than keep what it found.
        m_reloadQueued = true;
        m_queuedRoot = pioneerRoot;
        return;
    }
    m_pioneerRoot = pioneerRoot;
    attachSession();
    setErrorMessage({});
    setBusy(true);
    m_watcher.setFuture(runRead([pioneerRoot] { return runLoadTask(pioneerRoot); }));
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
        const QVariantMap group = groupVariant.toMap();
        for (const QVariant &fieldVariant : group["fields"].toList()) {
            QVariantMap field = fieldVariant.toMap();
            if (field["fileName"].toString() == fileName && field["label"].toString() == fieldLabel) {
                oldValue = field["value"].toString();
                known = field["options"].toStringList().contains(optionName);
            }
        }
    }
    if (!known) {
        setErrorMessage("\"" + fieldLabel + "\" cannot be set to " + optionName + ": not a value Seabass knows.");
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

void SettingsController::reconcileStaged()
{
    QStringList gone;
    QStringList already;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        const QString &fileName = it->first.first;
        const QString &label = it->first.second;
        const QString id = changeIdFor(fileName, label);
        std::optional<QString> onStick;
        for (const QVariant &groupVariant : m_groups) {
            for (const QVariant &fieldVariant : groupVariant.toMap()["fields"].toList()) {
                const QVariantMap field = fieldVariant.toMap();
                if (field["fileName"].toString() == fileName && field["label"].toString() == label) {
                    onStick = field["value"].toString();
                }
            }
        }
        if (!onStick) {
            // Not on the stick any more: there is no row left to show it
            // on, and nothing it could be written to. Said, not hidden.
            gone << label;
        } else if (*onStick == it->second) {
            // The stick already holds it: no "was X" against X.
            already << label;
        } else {
            // Staged again against what the stick holds now, so its "was"
            // and the change's own old value are the stick's.
            if (m_session) {
                m_session->stage(
                    std::make_unique<DeviceSettingChange>(m_pioneerRoot, fileName, label, *onStick, it->second));
            }
            ++it;
            continue;
        }
        if (m_session) {
            m_session->unstage(id);
        }
        it = m_pending.erase(it);
    }
    QStringList said;
    if (!gone.isEmpty()) {
        said << "Not on the stick any more, so no longer staged: " + gone.join(", ") + ".";
    }
    if (!already.isEmpty()) {
        said << "Already set that way on the stick, so no longer staged: " + already.join(", ") + ".";
    }
    setStatusMessage(said.join(" "));
}

void SettingsController::rebuildGroupsView()
{
    QVariantList view;
    for (const QVariant &groupVariant : m_groups) {
        QVariantMap group = groupVariant.toMap();
        QVariantList fields;
        for (const QVariant &fieldVariant : group["fields"].toList()) {
            QVariantMap field = fieldVariant.toMap();
            auto it = m_pending.find({field["fileName"].toString(), field["label"].toString()});
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
    QString thrown;
    SettingsTaskResult result = takeResult(m_watcher, &thrown);
    if (!thrown.isEmpty()) {
        result.errorMessage = thrown;
    }
    m_groups = result.groups;
    // Only a read that found the settings: one that failed (the stick gone
    // again) says nothing about what is staged, and the next read squares it.
    if (m_reconcileOnRead && result.errorMessage.isEmpty() && !m_groups.isEmpty()) {
        m_reconcileOnRead = false;
        reconcileStaged();
    }
    rebuildGroupsView();
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
    }
    setBusy(false);
    if (m_reloadQueued) {
        m_reloadQueued = false;
        load(m_queuedRoot);
    }
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
