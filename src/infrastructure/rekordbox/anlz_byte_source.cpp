// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/anlz_byte_source.hpp"

#include <fstream>
#include <sstream>

namespace seabass::infrastructure::rekordbox
{

std::string anlzRelativePath(const std::string &analyzePath, bool wantExt)
{
    std::string rel = analyzePath;
    const std::string marker = "/PIONEER/";
    size_t pos = rel.find(marker);
    if (pos != std::string::npos) {
        rel = rel.substr(pos + marker.size());
    }
    if (wantExt) {
        size_t dot = rel.rfind(".DAT");
        if (dot != std::string::npos) {
            rel = rel.substr(0, dot) + ".EXT";
        }
    }
    return rel;
}

FilesystemAnlzSource::FilesystemAnlzSource(std::string pioneerRoot) : m_pioneerRoot(std::move(pioneerRoot)) {}

std::optional<std::string> FilesystemAnlzSource::read(const std::string &relativePath)
{
    std::ifstream ifs(m_pioneerRoot + "/" + relativePath, std::ifstream::binary);
    if (!ifs.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << ifs.rdbuf();
    return buffer.str();
}

}  // namespace seabass::infrastructure::rekordbox
