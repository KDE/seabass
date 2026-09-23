// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
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
    explicit FileEntrySource(const std::filesystem::path &path) : m_path(path), m_in(path, std::ios::binary) {}

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
        if (m_readFailed) {
            return 0;  // the device refused; nothing after the refusal is this file's next bytes
        }
        if (!m_careful) {
            m_in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
            const auto got = static_cast<std::size_t>(m_in.gcount());
            m_offset += got;
            if (!m_in.bad()) {
                return got;
            }
            if (!openCareful()) {
                m_readFailed = true;
                return got;
            }
            return got + readCarefully(out.subspan(got));
        }
        return readCarefully(out);
    }

private:
    // A read that fails loses more than the bytes that could not be read.
    // The kernel reads ahead in windows of 128 KiB and more, one refused
    // page fails the whole window, and a 1 MiB read that runs into it
    // hands back nothing of the pages before the refusal either. Measured
    // on a stick with its FAT chains cut (issue #36): export.pdb had
    // 96 KiB readable and the archive got 0; a track had 25.9 MiB
    // readable and got 25.0.
    //
    // So after the first failure the rest of the file is read a page at
    // a time, from where the good bytes ended, through an unbuffered
    // stream so that each request is exactly one page, until the page
    // that really refuses. Only after a failure: a read that succeeds
    // never gets here.
    static constexpr std::size_t Page = 4096;

    bool openCareful()
    {
        m_careful.emplace();
        m_careful->rdbuf()->pubsetbuf(nullptr, 0);
        m_careful->open(m_path, std::ios::binary);
        m_careful->seekg(static_cast<std::streamoff>(m_offset));
        return static_cast<bool>(*m_careful);
    }

    std::size_t readCarefully(std::span<std::byte> out)
    {
        std::size_t got = 0;
        while (got < out.size()) {
            const std::size_t want = std::min(Page, out.size() - got);
            m_careful->read(reinterpret_cast<char *>(out.data() + got), static_cast<std::streamsize>(want));
            const auto n = static_cast<std::size_t>(m_careful->gcount());
            got += n;
            m_offset += n;
            if (m_careful->bad()) {
                m_readFailed = true;
                break;
            }
            if (n < want) {
                break;  // the end of the file
            }
        }
        return got;
    }

    std::filesystem::path m_path;
    std::ifstream m_in;
    std::optional<std::ifstream> m_careful;
    std::uint64_t m_offset = 0;
    bool m_readFailed = false;
};

}  // namespace seabass::infrastructure::stick_backup
