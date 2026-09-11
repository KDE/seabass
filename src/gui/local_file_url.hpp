// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QUrl>

#include <string>

namespace seabass::gui
{

// Converts a native local path (e.g. "H:\PIONEER/Contents/..." -- rekordbox
// stores its own paths with forward slashes regardless of platform, and
// readers just prepend the platform's native stick root) into a properly
// formed file:// URL string for QML's Image.source. Plain "file://" + path
// string concatenation works by accident on Linux (an absolute POSIX path
// already starts with '/', so the result happens to have the URL's required
// triple slash) but produces a malformed URL on Windows (missing the third
// slash before the drive letter, and containing raw backslashes) -- QUrl::
// fromLocalFile() handles both platforms correctly. Empty in, empty out.
inline QString toLocalFileUrl(const std::string &path)
{
    if (path.empty()) {
        return {};
    }
    return QUrl::fromLocalFile(QString::fromStdString(path)).toString();
}

// The inverse, for what a QML FileDialog/FolderDialog hands back: a
// file:// URL string, or an already-local path (a setting restored from
// disk, a test). Converted with QUrl::toLocalFile, never by stripping the
// scheme -- that leaves "/C:/..." on Windows and "%23" for a '#' in the
// name everywhere. Every dialog result in the app comes through here.
inline QString localPathFromUrl(const QString &pathOrUrl)
{
    if (pathOrUrl.startsWith(QStringLiteral("file:"))) {
        return QUrl(pathOrUrl).toLocalFile();
    }
    return pathOrUrl;
}

}  // namespace seabass::gui
