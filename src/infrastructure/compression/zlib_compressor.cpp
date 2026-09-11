// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/compression/zlib_compressor.hpp"

#include <stdexcept>
#include <vector>

#include <zlib.h>

namespace seabass::infrastructure::compression
{

std::string compress(const std::string &data)
{
    if (data.empty()) {
        return {};
    }
    uLongf boundLen = compressBound(static_cast<uLong>(data.size()));
    std::vector<Bytef> buffer(boundLen);

    uLongf destLen = boundLen;
    int result = compress2(buffer.data(), &destLen, reinterpret_cast<const Bytef *>(data.data()),
                            static_cast<uLong>(data.size()), Z_DEFAULT_COMPRESSION);
    if (result != Z_OK) {
        throw std::runtime_error("zlib compress2 failed with code " + std::to_string(result));
    }
    return std::string(reinterpret_cast<const char *>(buffer.data()), destLen);
}

std::string decompress(const std::string &data, size_t originalSize)
{
    if (originalSize == 0) {
        return {};
    }
    std::vector<Bytef> buffer(originalSize);
    uLongf destLen = static_cast<uLongf>(originalSize);
    int result = uncompress(buffer.data(), &destLen, reinterpret_cast<const Bytef *>(data.data()),
                             static_cast<uLong>(data.size()));
    if (result != Z_OK) {
        throw std::runtime_error("zlib uncompress failed with code " + std::to_string(result));
    }
    if (destLen != originalSize) {
        throw std::runtime_error("zlib uncompress produced an unexpected size");
    }
    return std::string(reinterpret_cast<const char *>(buffer.data()), destLen);
}

}  // namespace seabass::infrastructure::compression
