// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/staged_edits_store.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "application/stick_identity.hpp"
#include "infrastructure/local/app_data_directory.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

namespace
{

QString fromPath(const std::filesystem::path &path)
{
    return QString::fromStdString(seabass::pathToGenericUtf8(path));
}

}  // namespace

StagedEditsStore::StagedEditsStore()
    : StagedEditsStore(fromPath(infrastructure::local::appDataDirectory() / "staged-edits"))
{
}

StagedEditsStore::StagedEditsStore(QString directory) : m_directory(std::move(directory))
{
}

QString StagedEditsStore::pathFor(const QString &libraryId) const
{
    return m_directory + QLatin1Char('/')
        + QString::fromStdString(application::StickIdentity::sanitizeForFileName(libraryId.toStdString()))
        + QStringLiteral(".json");
}

QString StagedEditsStore::relativeTo(const QString &mountPoint, const QString &path)
{
    const QString root = QDir::fromNativeSeparators(mountPoint);
    const QString file = QDir::fromNativeSeparators(path);
    if (root.isEmpty() || file.isEmpty()) {
        return path;
    }
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return file.startsWith(prefix) ? file.mid(prefix.size()) : path;
}

QString StagedEditsStore::reroot(const QString &mountPoint, const QString &relative)
{
    if (relative.isEmpty() || mountPoint.isEmpty() || QDir::isAbsolutePath(relative)) {
        return relative;
    }
    const QString root = QDir::fromNativeSeparators(mountPoint);
    return QDir::toNativeSeparators((root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/')) + relative);
}

bool StagedEditsStore::write(const StagedEdits &edits, const QString &mountPoint) const
{
    if (edits.libraryId.isEmpty()) {
        return false;
    }
    if (edits.requests.empty()) {
        remove(edits.libraryId);
        return true;
    }
    QJsonArray list;
    for (const StagedEditRequest &r : edits.requests) {
        list.append(QJsonObject{{QStringLiteral("kind"), r.kind},
                                {QStringLiteral("playlist"), r.playlist},
                                {QStringLiteral("file"), relativeTo(mountPoint, r.file)},
                                {QStringLiteral("title"), r.title},
                                {QStringLiteral("toIndex"), r.toIndex},
                                {QStringLiteral("newName"), r.newName},
                                {QStringLiteral("anyway"), r.anyway}});
    }
    const QJsonObject root{{QStringLiteral("libraryId"), edits.libraryId},
                           {QStringLiteral("stickLabel"), edits.stickLabel},
                           {QStringLiteral("savedAtUtc"), edits.savedAtUtc},
                           {QStringLiteral("appVersion"), edits.appVersion},
                           {QStringLiteral("edits"), list}};
    if (!QDir().mkpath(m_directory)) {
        return false;
    }
    // Whole or not at all: a crash in the middle of this write must not
    // take the snapshot before it along.
    QSaveFile file(pathFor(edits.libraryId));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

std::optional<StagedEdits> StagedEditsStore::read(const QString &libraryId, const QString &mountPoint) const
{
    if (libraryId.isEmpty()) {
        return std::nullopt;
    }
    QFile file(pathFor(libraryId));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    StagedEdits edits;
    edits.libraryId = root.value(QStringLiteral("libraryId")).toString();
    edits.stickLabel = root.value(QStringLiteral("stickLabel")).toString();
    edits.savedAtUtc = root.value(QStringLiteral("savedAtUtc")).toString();
    edits.appVersion = root.value(QStringLiteral("appVersion")).toString();
    for (const QJsonValue &value : root.value(QStringLiteral("edits")).toArray()) {
        const QJsonObject o = value.toObject();
        StagedEditRequest r;
        r.kind = o.value(QStringLiteral("kind")).toString();
        r.playlist = o.value(QStringLiteral("playlist")).toString();
        r.file = reroot(mountPoint, o.value(QStringLiteral("file")).toString());
        r.title = o.value(QStringLiteral("title")).toString();
        r.toIndex = o.value(QStringLiteral("toIndex")).toInt(-1);
        r.newName = o.value(QStringLiteral("newName")).toString();
        r.anyway = o.value(QStringLiteral("anyway")).toBool();
        if (!r.kind.isEmpty()) {
            edits.requests.push_back(r);
        }
    }
    // Another library's file under this name would be another stick's
    // edits: never offered for this one.
    if (edits.requests.empty() || edits.libraryId != libraryId) {
        return std::nullopt;
    }
    return edits;
}

void StagedEditsStore::remove(const QString &libraryId) const
{
    if (!libraryId.isEmpty()) {
        QFile::remove(pathFor(libraryId));
    }
}

}  // namespace seabass::gui
