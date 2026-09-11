// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/direct_write_hold.hpp"

#include "gui/edit/edit_session_registry.hpp"

namespace seabass::gui
{

DirectWriteHold::~DirectWriteHold()
{
    release();
}

std::optional<DirectWriteHold::Refusal> DirectWriteHold::acquire(const QStringList &libraryIds,
                                                                 const QString &stickLabel,
                                                                 std::function<void()> retry)
{
    release();
    auto *registry = EditSessionRegistry::instance();
    for (const QString &id : libraryIds) {
        if (id.isEmpty() || m_held.contains(id)) {
            continue;
        }
        if (auto refusal = registry->enterDirectWrite(id, stickLabel)) {
            release();
            m_refusedLibraryId = id;
            m_retry = std::move(retry);
            return refusal;
        }
        m_held.push_back(id);
    }
    m_refusedLibraryId.clear();
    m_retry = nullptr;
    return std::nullopt;
}

void DirectWriteHold::release()
{
    if (m_held.isEmpty()) {
        return;
    }
    auto *registry = EditSessionRegistry::instance();
    for (const QString &id : m_held) {
        registry->leaveDirectWrite(id);
    }
    m_held.clear();
}

void DirectWriteHold::retryLockedAction()
{
    std::function<void()> retry = std::move(m_retry);
    m_retry = nullptr;
    if (retry) {
        retry();
    }
}

}  // namespace seabass::gui
