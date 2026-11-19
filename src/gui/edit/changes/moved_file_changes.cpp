// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/moved_file_changes.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "application/path_key.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

constexpr const char *MoveLabel = "moved-files";
constexpr const char *RelinkLabel = "moved-files-relink";

std::string exportPdbOf(const std::string &pioneerRoot)
{
    return pathToUtf8(pathFromUtf8(pioneerRoot) / "rekordbox" / "export.pdb");
}

std::string engineDbOf(const std::string &enginePath)
{
    return pathToUtf8(pathFromUtf8(enginePath) / "Database2" / "m.db");
}

std::string basenameOf(const std::string &path)
{
    return pathToUtf8(pathFromUtf8(path).filename());
}

QString nameOf(const domain::MovedFileFinding &finding)
{
    const std::string title = finding.title();
    return QString::fromStdString(title.empty() ? basenameOf(finding.expectedPath) : title);
}

// The catalogs this finding has rows in, as formatsTouched() lists them.
QStringList formatsOf(const domain::MovedFileFinding &finding)
{
    QStringList formats;
    for (const auto &row : finding.rows) {
        const QString format = QString::fromStdString(row.format);
        if (!formats.contains(format)) {
            formats << format;
        }
    }
    return formats;
}

}  // namespace

QString movedFileChangeId(const std::string &expectedPath)
{
    return "moved-file:" + QString::fromStdString(application::normalizedPathKey(expectedPath));
}

std::string rekordboxPathOnStick(const std::string &pioneerRoot, const std::string &filePath)
{
    std::error_code ec;
    const auto relative = fs::relative(pathFromUtf8(filePath), pathFromUtf8(pioneerRoot).parent_path(), ec);
    return ec || relative.empty() ? filePath : "/" + pathToGenericUtf8(relative);
}

std::string relinkRefusalFor(const QString &pioneerRoot, const domain::MovedFileFinding &finding,
                             const std::string &newPath)
{
    bool rekordbox = false;
    for (const auto &row : finding.rows) {
        rekordbox = rekordbox || row.format == "rekordbox";
    }
    if (!rekordbox) {
        return {};  // Engine and OneLibrary take any path
    }
    const std::string pioneer = pioneerRoot.toStdString();
    const std::string stored = rekordboxPathOnStick(pioneer, newPath);
    const std::string name = basenameOf(newPath);
    for (unsigned char c : stored) {
        if (c >= 0x80) {
            return "export.pdb takes only plain ASCII in a track's path, and \"" + stored + "\" is not";
        }
    }
    try {
        const infrastructure::rekordbox::PdbRowWriter pdb(exportPdbOf(pioneer));
        for (const auto &row : finding.rows) {
            if (row.format != "rekordbox") {
                continue;
            }
            const auto capacity = pdb.trackPathCapacity(static_cast<uint32_t>(std::stoul(row.sourceId)));
            if (!capacity) {
                return "this track's path fields in export.pdb cannot be written in place";
            }
            if (stored.size() > capacity->filePath) {
                return "the new path is " + std::to_string(stored.size()) + " characters, and the track's path field "
                       "in export.pdb holds " + std::to_string(capacity->filePath)
                       + "; a row cannot grow without rewriting its page";
            }
            if (name.size() > capacity->filename) {
                return "the new filename is " + std::to_string(name.size()) + " characters, and the track's filename "
                       "field in export.pdb holds " + std::to_string(capacity->filename);
            }
        }
    } catch (const std::exception &e) {
        return std::string("export.pdb could not be read: ") + e.what();
    }
    return {};
}

// ---- Repair A ----------------------------------------------------------

RelocateMovedFileChange::RelocateMovedFileChange(domain::MovedFileFinding finding, domain::Track file)
    : m_finding(std::move(finding)), m_file(std::move(file))
{
}

QString RelocateMovedFileChange::id() const
{
    return movedFileChangeId(m_finding.expectedPath);
}

QString RelocateMovedFileChange::owner() const
{
    return QStringLiteral("library-health");
}

QString RelocateMovedFileChange::description() const
{
    return QStringLiteral("Move \"%1\" back to %2")
        .arg(nameOf(m_finding), QString::fromStdString(m_finding.expectedPath));
}

QString RelocateMovedFileChange::subject() const
{
    return nameOf(m_finding);
}

QString RelocateMovedFileChange::unit() const
{
    return QStringLiteral("files");
}

QString RelocateMovedFileChange::verb() const
{
    return QStringLiteral("moved back");
}

// No catalog is written: the three stay level because the file goes
// where they all already point.
QStringList RelocateMovedFileChange::formatsTouched() const
{
    return {};
}

ChangeOutcome RelocateMovedFileChange::apply(SaveContext &ctx)
{
    const fs::path from = pathFromUtf8(m_file.filePath);
    const fs::path to = pathFromUtf8(m_finding.expectedPath);
    const std::string label = MoveLabel;
    std::error_code ec;
    if (fs::is_regular_file(to, ec)) {
        // Something is there again: the catalogs resolve, whatever it is,
        // and the next scan says whether it is the file.
        ctx.log().record(label + ": a file is at " + m_finding.expectedPath + " again; nothing moved");
        return ChangeOutcome::skip();
    }
    if (fs::exists(to, ec) || ec) {
        return ChangeOutcome::failure(
            QStringLiteral("Something that is not a file is at %1, so the file cannot be moved back there.")
                .arg(QString::fromStdString(m_finding.expectedPath)));
    }
    if (!fs::is_regular_file(from, ec)) {
        ctx.log().record(label + ": " + m_file.filePath + " is no longer there; nothing moved");
        return ChangeOutcome::skip();
    }
    // The file the scan matched, not another put at its path since: the
    // size is what made the match.
    const std::uintmax_t size = fs::file_size(from, ec);
    if (!ec && m_file.fileSizeBytes > 0 && size != m_file.fileSizeBytes) {
        return ChangeOutcome::failure(QStringLiteral("%1 has changed since the scan (%2 bytes, the scan saw %3), "
                                                     "so it is not moved. Run the check again.")
                                          .arg(QString::fromStdString(m_file.filePath))
                                          .arg(static_cast<qulonglong>(size))
                                          .arg(static_cast<qulonglong>(m_file.fileSizeBytes)));
    }
    // The folder the catalogs expect may be the thing that was renamed.
    // Making it again is part of putting the file back; Undo leaves it
    // standing, empty, which no catalog minds.
    if (!fs::is_directory(to.parent_path(), ec)) {
        fs::create_directories(to.parent_path(), ec);
        if (ec) {
            return ChangeOutcome::failure(QStringLiteral("Could not make the folder %1: %2")
                                              .arg(pathToQString(to.parent_path()),
                                                   QString::fromStdString(ec.message())));
        }
        ctx.log().record(label + ": made the folder " + pathToUtf8(to.parent_path()));
    }
    // Noted before the move (see SaveContext::noteMoved), so Undo and a
    // failed save know to move it back.
    ctx.noteMoved(m_file.filePath, m_finding.expectedPath, label);
    fs::rename(from, to, ec);
    if (ec) {
        return ChangeOutcome::failure(QStringLiteral("Could not move %1 to %2: %3")
                                          .arg(QString::fromStdString(m_file.filePath),
                                               QString::fromStdString(m_finding.expectedPath),
                                               QString::fromStdString(ec.message())));
    }
    ctx.log().record(label + ": moved " + m_file.filePath + " back to " + m_finding.expectedPath + " (\""
                     + m_finding.title() + "\")");
    return ChangeOutcome::success();
}

// ---- Repair B ----------------------------------------------------------

RelinkMovedFileChange::RelinkMovedFileChange(QString pioneerRoot, QString enginePath,
                                             domain::MovedFileFinding finding, domain::Track file)
    : m_pioneerRoot(std::move(pioneerRoot)),
      m_enginePath(std::move(enginePath)),
      m_finding(std::move(finding)),
      m_file(std::move(file))
{
}

QString RelinkMovedFileChange::id() const
{
    return movedFileChangeId(m_finding.expectedPath);
}

QString RelinkMovedFileChange::owner() const
{
    return QStringLiteral("library-health");
}

QString RelinkMovedFileChange::description() const
{
    return QStringLiteral("Point the %1 entries for \"%2\" at %3")
        .arg(formatsOf(m_finding).join(QStringLiteral(", ")), nameOf(m_finding),
             QString::fromStdString(m_file.filePath));
}

QString RelinkMovedFileChange::subject() const
{
    return nameOf(m_finding);
}

QString RelinkMovedFileChange::unit() const
{
    return QStringLiteral("tracks");
}

QString RelinkMovedFileChange::verb() const
{
    return QStringLiteral("relinked");
}

QStringList RelinkMovedFileChange::formatsTouched() const
{
    return formatsOf(m_finding);
}

bool RelinkMovedFileChange::has(const std::string &format) const
{
    for (const auto &row : m_finding.rows) {
        if (row.format == format) {
            return true;
        }
    }
    return false;
}

std::vector<BackupTarget> RelinkMovedFileChange::filesToBackup(SaveContext &ctx) const
{
    (void)ctx;
    std::vector<BackupTarget> targets;
    const std::string pioneer = m_pioneerRoot.toStdString();
    if (has("rekordbox") && !pioneer.empty()) {
        targets.push_back({exportPdbOf(pioneer), RelinkLabel});
    }
    if (has("onelibrary") && !pioneer.empty()) {
        targets.push_back({infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), RelinkLabel});
    }
    if (has("engine") && !m_enginePath.isEmpty()) {
        targets.push_back({engineDbOf(m_enginePath.toStdString()), RelinkLabel});
    }
    return targets;
}

ChangeOutcome RelinkMovedFileChange::apply(SaveContext &ctx)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    const std::string engine = m_enginePath.toStdString();
    const std::string label = RelinkLabel;
    const std::string &oldPath = m_finding.expectedPath;
    const std::string &newPath = m_file.filePath;
    std::error_code ec;
    if (!fs::is_regular_file(pathFromUtf8(newPath), ec)) {
        ctx.log().record(label + ": " + newPath + " is no longer there; nothing pointed at it");
        return ChangeOutcome::skip();
    }
    int pointed = 0;
    try {
        if (has("rekordbox")) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, 1, label);
            infrastructure::rekordbox::PdbRowWriter pdb(exportPdbOf(session.writeRoot()));
            // By the path, not the id from the scan: a re-export renumbers.
            const std::vector<uint32_t> ids = pdb.trackIdsWithFilePath(rekordboxPathOnStick(pioneer, oldPath));
            const std::string stored = rekordboxPathOnStick(pioneer, newPath);
            const std::string name = basenameOf(newPath);
            bool changed = false;
            for (const uint32_t id : ids) {
                std::string refusal;
                if (pdb.setTrackFilePath(id, name, stored, &refusal)) {
                    changed = true;
                    ++pointed;
                    continue;
                }
                if (!refusal.empty()) {
                    return ChangeOutcome::failure(
                        QStringLiteral("export.pdb cannot be pointed at %1: %2. Moving the file back instead works "
                                       "for every catalog.")
                            .arg(QString::fromStdString(newPath), QString::fromStdString(refusal)));
                }
            }
            if (changed) {
                if (!pdb.commit()) {
                    return ChangeOutcome::failure(QStringLiteral("could not write export.pdb"));
                }
                session.noteItemApplied();
            } else {
                ctx.log().record(label + ": no rekordbox row names " + oldPath + " any more");
            }
        }
        if (has("onelibrary")) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), label);
            try {
                pointed += sharedOneLibraryWriter(ctx, pioneer).setTrackPath(oldPath, newPath);
            } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                ctx.log().record(label + ": no OneLibrary row names " + oldPath + " any more");
            }
        }
        if (has("engine")) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "engine", engine, 1, label);
            const int rows =
                infrastructure::engine::setEngineTrackPath(session.writeRoot(), session.realRoot(), oldPath, newPath);
            if (rows > 0) {
                session.noteItemApplied();
                pointed += rows;
            } else {
                ctx.log().record(label + ": no Engine row names " + oldPath + " any more");
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    if (pointed == 0) {
        return ChangeOutcome::skip();
    }
    ctx.log().record(label + ": pointed " + std::to_string(pointed) + " row(s) for \"" + m_finding.title() + "\" at "
                     + newPath + " (was " + oldPath + ")");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
