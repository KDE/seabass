// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "domain/moved_files.hpp"
#include "domain/track.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// The two repairs of a file that moved on the stick (#103, Library
// Health's "Moved files"). Both carry the finding and the one file chosen
// for it, and both have the same id for one expected path, so staging
// one replaces the other: a track gets one of the two, never both.
QString movedFileChangeId(const std::string &expectedPath);

// Repair A, the default: the file moved back to the path every catalog
// expects. No database is written, so rekordbox, OneLibrary and Engine
// stay level by construction, and Undo is a move back (SaveContext::
// noteMoved). A file that is no longer where the scan found it, or a
// path that has something at it again, is a skip, not a failure: the
// stick changed since the scan, and the next scan says how.
class RelocateMovedFileChange : public PendingChange
{
public:
    RelocateMovedFileChange(domain::MovedFileFinding finding, domain::Track file);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    domain::MovedFileFinding m_finding;
    domain::Track m_file;
};

// Repair B: every catalog row pointed at the file where it is now.
// Engine's Track.path and filename and OneLibrary's content.path and
// fileName are plain updates; export.pdb's file_path and filename are
// overwritten in the span their old values take
// (PdbRowWriter::setTrackFilePath), which writes them exactly or refuses.
// Offered only when relinkRefusalFor() is empty, which the page checks
// before it offers it; checked again here, since the catalog may have
// changed since the scan. Each catalog's rows are found by the spelling
// of that catalog's own row (the finding groups spellings that differ in
// case or normalization), and a row the finding lists that is not found
// fails the change, so the save puts back whatever it had written: the
// catalogs never end up pointing at two places. Refused, too, when the
// file is not the size the scan saw, or a file is at the old path again.
class RelinkMovedFileChange : public PendingChange
{
public:
    RelinkMovedFileChange(QString pioneerRoot, QString enginePath, domain::MovedFileFinding finding,
                          domain::Track file);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    bool has(const std::string &format) const;

    QString m_pioneerRoot;
    QString m_enginePath;
    domain::MovedFileFinding m_finding;
    domain::Track m_file;
};

// Why pointing the catalogs at `newPath` cannot be offered for this
// finding, or empty when it can: export.pdb takes only plain ASCII, and
// only a path and filename of exactly the lengths the row holds. Reads
// export.pdb's row for each rekordbox row of the finding (a whole-file
// read, once per call), so it is for a scan, not a binding.
std::string relinkRefusalFor(const QString &pioneerRoot, const domain::MovedFileFinding &finding,
                             const std::string &newPath);

// "/Contents/..." as export.pdb spells a file on the stick whose PIONEER
// folder is `pioneerRoot`.
std::string rekordboxPathOnStick(const std::string &pioneerRoot, const std::string &filePath);

}  // namespace seabass::gui
