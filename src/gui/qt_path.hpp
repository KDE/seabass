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
namespace seabass::gui
{

inline std::filesystem::path pathFromQString(const QString &path)
{
    return pathFromUtf8(path.toStdString());
}

inline QString pathToQString(const std::filesystem::path &path)
{
    return QString::fromStdString(pathToUtf8(path));
}

}  // namespace seabass::gui
