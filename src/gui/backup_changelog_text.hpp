// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <filesystem>

namespace seabass::gui
{

// An archive's own history, as a plain text file someone can open.
//
// The manifest keeps it as tab-separated rows because that is what the
// archive format needs; nobody wants to read that. This renders it the
// way a person would write it down -- newest first, dates spelled out,
// byte counts in units -- and returns the path of a file to hand to the
// system's text editor.
//
// Written to a temporary file rather than beside the archive, because
// the backup folder is the user's and this is a view of the backup, not
// a part of it: anything written there would show up in the next
// listing, and something that looks like a second backup next to a
// backup is exactly the confusion to avoid.
//
// Both pages that list backups use this, so the two cannot drift into
// showing the same archive's history differently.
//
// Returns an empty string on failure, with the reason in `error`: an
// archive that will not open, has no manifest, or has no history in it
// yet (every backup written before the log existed).
QString writeChangelogFile(const std::filesystem::path &archivePath, QString *error);

}  // namespace seabass::gui
