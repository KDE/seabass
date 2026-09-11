// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/long_paths.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace seabass::infrastructure
{

namespace fs = std::filesystem;

#if defined(_WIN32)

struct DirectoryReader::State
{
    HANDLE find = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW data{};
    bool pending = false;  // `data` holds an entry that next() has not returned yet
    fs::path directory;    // already longPathSafe'd; children are joined onto it
};

DirectoryReader::DirectoryReader(const fs::path &directory, std::error_code &ec) : m_state(std::make_unique<State>())
{
    ec.clear();
    m_state->directory = longPathSafe(directory);
    const std::wstring pattern = m_state->directory.native() + L"\\*";
    m_state->find = ::FindFirstFileW(pattern.c_str(), &m_state->data);
    if (m_state->find == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        // An empty directory always has "." and "..", so ERROR_FILE_NOT_FOUND
        // here means the directory itself is not readable, not that it is
        // empty. ERROR_ACCESS_DENIED is left to the caller to report, the
        // way directory_options::skip_permission_denied would.
        ec = std::error_code(static_cast<int>(error), std::system_category());
        return;
    }
    m_state->pending = true;
}

DirectoryReader::~DirectoryReader()
{
    if (m_state && m_state->find != INVALID_HANDLE_VALUE) {
        ::FindClose(m_state->find);
    }
}

bool DirectoryReader::next(fs::path &child, std::error_code &ec)
{
    ec.clear();
    if (m_state->find == INVALID_HANDLE_VALUE) {
        return false;
    }
    for (;;) {
        if (!m_state->pending) {
            if (!::FindNextFileW(m_state->find, &m_state->data)) {
                const DWORD error = ::GetLastError();
                if (error != ERROR_NO_MORE_FILES) {
                    ec = std::error_code(static_cast<int>(error), std::system_category());
                }
                return false;
            }
        }
        m_state->pending = false;
        const std::wstring name = m_state->data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        child = m_state->directory / name;
        return true;
    }
}

#else

struct DirectoryReader::State
{
    fs::directory_iterator it;
};

DirectoryReader::DirectoryReader(const fs::path &directory, std::error_code &ec) : m_state(std::make_unique<State>())
{
    m_state->it = fs::directory_iterator(directory, fs::directory_options::skip_permission_denied, ec);
}

DirectoryReader::~DirectoryReader() = default;

bool DirectoryReader::next(fs::path &child, std::error_code &ec)
{
    ec.clear();
    if (m_state->it == fs::directory_iterator()) {
        return false;
    }
    child = m_state->it->path();
    m_state->it.increment(ec);
    if (ec) {
        return false;
    }
    return true;
}

#endif

std::uintmax_t removeTreeDeepestFirst(const fs::path &path)
{
    std::error_code ec;
    const fs::path full = longPathSafe(path);
    const fs::file_status status = fs::symlink_status(full, ec);
    if (ec || !fs::exists(status)) {
        return 0;
    }
    std::uintmax_t removed = 0;
    if (fs::is_directory(status) && !fs::is_symlink(status)) {
        DirectoryReader reader(full, ec);
        if (!ec) {
            fs::path child;
            std::error_code readEc;
            while (reader.next(child, readEc)) {
                removed += removeTreeDeepestFirst(child);
            }
        }
    }
    std::error_code removeEc;
    if (fs::remove(full, removeEc)) {
        ++removed;
    }
    return removed;
}

std::uintmax_t directoryTreeSizeBytes(const fs::path &path)
{
    std::error_code ec;
    const fs::path full = longPathSafe(path);
    const fs::file_status status = fs::symlink_status(full, ec);
    if (ec || !fs::exists(status) || fs::is_symlink(status)) {
        return 0;
    }
    if (fs::is_regular_file(status)) {
        const std::uintmax_t size = fs::file_size(full, ec);
        return ec ? 0 : size;
    }
    if (!fs::is_directory(status)) {
        return 0;
    }
    DirectoryReader reader(full, ec);
    if (ec) {
        return 0;
    }
    std::uintmax_t total = 0;
    fs::path child;
    std::error_code readEc;
    while (reader.next(child, readEc)) {
        total += directoryTreeSizeBytes(child);
    }
    return total;
}

}  // namespace seabass::infrastructure
