// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/anlz_file.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace fs = std::filesystem;

namespace
{

// Validates the section-level framing of a full ANLZ file's raw bytes
// (magic, len_file consistency, len_header bounds, well-formed section
// headers) without building an AnlzFile from it -- shared by readRaw()
// (validating what was read from disk) and writeRaw() (validating what
// this class is about to write, before it ever reaches disk, so a bug
// in this class producing a broken file can never get that far).
void validateAnlzBytes(const std::string &data, const std::string &context)
{
    if (data.size() < 12 || data.substr(0, 4) != "PMAI") {
        throw std::runtime_error(context + " is not an ANLZ file (bad magic)");
    }
    uint32_t lenHeader = readU32BE(data, 4);
    uint32_t lenFile = readU32BE(data, 8);
    if (lenFile != data.size()) {
        throw std::runtime_error(context + ": len_file (" + std::to_string(lenFile) +
                                  ") doesn't match actual file size (" + std::to_string(data.size()) + ")");
    }
    if (lenHeader > data.size()) {
        throw std::runtime_error(context + ": len_header is larger than the file");
    }

    size_t pos = lenHeader;
    while (pos < data.size()) {
        if (pos + 12 > data.size()) {
            throw std::runtime_error(context + ": truncated section header at offset " + std::to_string(pos));
        }
        uint32_t lenTag = readU32BE(data, pos + 8);
        if (lenTag < 12 || pos + lenTag > data.size()) {
            throw std::runtime_error(context + ": invalid section length at offset " + std::to_string(pos));
        }
        pos += lenTag;
    }
}

}  // namespace

AnlzFile AnlzFile::readRaw(const std::string &path)
{
    std::ifstream ifs(path, std::ifstream::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("could not open " + path);
    }
    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    validateAnlzBytes(data, path);

    uint32_t lenHeader = readU32BE(data, 4);

    AnlzFile result;
    result.headerBytes = data.substr(0, lenHeader);

    size_t pos = lenHeader;
    while (pos < data.size()) {
        uint32_t fourcc = readU32BE(data, pos);
        uint32_t lenTag = readU32BE(data, pos + 8);
        result.sections.push_back({fourcc, data.substr(pos, lenTag)});
        pos += lenTag;
    }

    result.m_sourcePath = path;
    std::error_code ec;
    result.m_sourceFileSize = fs::file_size(path, ec);
    result.m_sourceMtime = fs::last_write_time(path, ec);
    return result;
}

void AnlzFile::writeRaw(const std::string &path) const
{
    // Staleness check: only meaningful when writing back to the same
    // path this instance was read from -- if the file changed on disk
    // in the meantime, something else touched it and this in-memory
    // copy is no longer a safe base to overwrite it with.
    if (!m_sourcePath.empty() && path == m_sourcePath) {
        std::error_code ec;
        auto currentSize = fs::file_size(path, ec);
        auto currentMtime = fs::last_write_time(path, ec);
        if (ec || currentSize != m_sourceFileSize || currentMtime != m_sourceMtime) {
            throw std::runtime_error(path + " changed on disk since it was read -- refusing to overwrite it with a stale copy");
        }
    }

    std::string out = headerBytes;
    for (const auto &section : sections) {
        out += section.rawBytes;
    }
    writeU32BE(out, 8, static_cast<uint32_t>(out.size()));

    // A bug in this class producing a broken file must never reach
    // disk -- confirm the reassembled bytes are still a valid ANLZ file
    // before writing them anywhere.
    validateAnlzBytes(out, path);

    if (!infrastructure::writeFileDurablyAtomic(path, out)) {
        throw std::runtime_error("failed to durably write " + path);
    }
}

}  // namespace seabass::infrastructure::rekordbox
