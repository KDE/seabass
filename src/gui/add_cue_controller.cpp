// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "add_cue_controller.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>

#include "application/ports/cue_writer.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/library_catalog_cache.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "gui/edit/changes/add_cue_change.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

AddCueController::AddCueController(QObject *parent) : QObject(parent) {}

bool AddCueController::writing() const
{
    return m_session && m_session->writing();
}

void AddCueController::attachSession(const QString &format, const QString &path)
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(path));
    if (session != m_session) {
        if (m_session) {
            disconnect(m_session, nullptr, this, nullptr);
        }
        m_session = session;
        if (m_session) {
            connect(m_session, &LibraryEditSession::stateChanged, this, &AddCueController::writingChanged);
            connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
                if (m_pending.erase(changeId) > 0) {
                    ++m_pendingRevision;
                    emit pendingChanged();
                }
            });
            connect(m_session, &LibraryEditSession::saveFinished, this, [this](const QVariantMap &summary) {
                if (summary.value("written").toInt() > 0) {
                    emit cuesSaved();
                }
            });
            connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
                if (!m_pending.empty()) {
                    m_pending.clear();
                    ++m_pendingRevision;
                    emit pendingChanged();
                }
            });
        }
    }
    if (m_session) {
        if (format == "engine") {
            m_session->setLibraryPaths(QString(), path);
        } else {
            m_session->setLibraryPaths(path, QString());
        }
    }
}

void AddCueController::addCue(const QString &format, const QString &path, const QString &sourceId, double positionMs,
                               const QString &kind, int hotCueNumber, const QString &color, const QString &comment,
                               bool isLoop, double loopEndMs, const QString &trackTitle)
{
    setErrorMessage({});
    setStatusMessage({});
    if (format != "rekordbox" && format != "engine" && format != "onelibrary") {
        setErrorMessage("Unknown library format: " + format);
        return;
    }
    if (isLoop && format != "engine") {
        // See the class comment: rekordbox's ANLZ loop encoding is
        // unverified against real hardware, and OneLibrary's cue table
        // has never been confirmed to round-trip loops either --
        // refusing here means a DJ never gets a cue silently written as
        // a point when they asked to save a loop-out.
        setErrorMessage("Hot loops can only be added to Engine tracks right now.");
        return;
    }
    if (isLoop && kind != "hot") {
        // Engine's loops() array is indexed exactly like hot_cues() --
        // there's no Engine concept of an un-slotted "memory loop" to
        // write this into.
        setErrorMessage("Loops need a hot cue slot.");
        return;
    }

    attachSession(format, path);
    if (!m_session) {
        setErrorMessage("This stick's library could not be identified; nothing was changed.");
        return;
    }
    if (m_session->writing()) {
        setErrorMessage("A save is running -- add the cue once it has finished.");
        return;
    }

    auto change = std::make_unique<AddCueChange>(format, path, sourceId, positionMs, kind, hotCueNumber, color, comment,
                                                 isLoop, loopEndMs, trackTitle);
    QString changeId = change->id();
    QString description = change->description();
    QVariantMap summary = change->summary();
    if (!m_session->stage(std::move(change))) {
        return;  // the session reported the lock refusal; the page shows it
    }
    m_pending[changeId] = summary;
    ++m_pendingRevision;
    emit pendingChanged();
    setStatusMessage("Staged: " + description + ". Press Save to write it to the stick.");
}

QVariantList AddCueController::pendingCuesFor(const QString &sourceId) const
{
    QVariantList list;
    for (const auto &[id, cue] : m_pending) {
        if (cue.value("sourceId").toString() == sourceId) {
            list << cue;
        }
    }
    return list;
}

void AddCueController::unstage(const QString &changeId)
{
    if (m_pending.erase(changeId) == 0) {
        return;
    }
    if (m_session) {
        m_session->unstage(changeId);
    }
    ++m_pendingRevision;
    emit pendingChanged();
    setStatusMessage({});
}

void AddCueController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void AddCueController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
