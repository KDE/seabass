// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "infrastructure/paths/utf8_path.hpp"

#include <QString>

#include <filesystem>

// QString <-> std::filesystem::path, by the rule in utf8_path.hpp. Never
// fs::path(q.toStdString()) or QString::fromStdString(p.string()): both
// go through the ANSI code page on Windows.
//
// A QString path has forward slashes on every platform, the way Qt's own
// file dialogs, QUrl::toLocalFile and QDir hand them out; pathToQString
// gives that form, so a path the app derived compares equal to one a
// page was given. On Windows the native form has backslashes, and the
// QML suite caught a session that knew "...\Engine Library" while its
// page said "...\Engine Library" with a slash. QDir::toNativeSeparators
// is for showing a path to a person, at the point of showing it.
namespace seabass::gui
{

inline std::filesystem::path pathFromQString(const QString &path)
{
    return pathFromUtf8(path.toStdString());
}

inline QString pathToQString(const std::filesystem::path &path)
{
    return QString::fromStdString(pathToGenericUtf8(path));
}

// A UTF-8 std::string path (a DetectedStick's, a use case's) as the
// QString a page holds: the same conversion, so the stick list and a
// path a page derived agree on the spelling.
inline QString qtPathFromUtf8(const std::string &utf8)
{
    return pathToQString(pathFromUtf8(utf8));
}

}  // namespace seabass::gui
