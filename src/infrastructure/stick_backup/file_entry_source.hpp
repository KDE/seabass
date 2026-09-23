// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <fstream>
#include <filesystem>
#include <span>

#include "infrastructure/stick_backup/archive_updater.hpp"

namespace seabass::infrastructure::stick_backup
{

// A file on the stick, read into the archive.
//
// There were two of these, identical, both born in the same commit
// (1aa153b4) -- one in backup_stick.cpp for the ordinary file loop and
// one in sqlite_db_set.cpp for database members. Nothing had made them
// disagree until readFailed() was added to the first for the salvage
// work and not to the second, and the difference cost a data-loss bug
// within a day: a database whose read stopped at 128 KiB of 268 KiB was
// stored as 0 bytes and reported as captured, because the copy that
// could not tell a refusal from an ending was the one reading it.
//
// So: one class, in a header both can reach. A hardening applied here
// is applied to every reader of a stick.
class FileEntrySource : public EntrySource
{
public:
    explicit FileEntrySource(const std::filesystem::path &path) : m_in(path, std::ios::binary) {}

    // Whether the file opened at all.
    bool ok() const { return static_cast<bool>(m_in); }

    // Whether a read FAILED, as against reaching the end of the file.
    // istream signals those two differently and the difference is the
    // point: eofbit is an ordinary finish, badbit is the device
    // refusing, and a stream that returns fewer bytes without badbit
    // has simply ended.
    //
    // Latched, because the caller reads in a loop and asks afterwards.
    bool readFailed() const { return m_readFailed; }

    std::size_t read(std::span<std::byte> out) override
    {
        m_in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
        if (m_in.bad()) {
            m_readFailed = true;
        }
        return static_cast<std::size_t>(m_in.gcount());
    }

private:
    std::ifstream m_in;
    bool m_readFailed = false;
};

}  // namespace seabass::infrastructure::stick_backup
