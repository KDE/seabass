// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/identical_file_alignment_probe.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::audio
{

bool filesHaveSameBytes(const std::string &absolutePathA, const std::string &absolutePathB)
{
    if (absolutePathA.empty() || absolutePathB.empty()) {
        return false;
    }
    namespace fs = std::filesystem;
    const fs::path pathA = pathFromUtf8(absolutePathA);
    const fs::path pathB = pathFromUtf8(absolutePathB);
    std::error_code ec;
    const auto sizeA = fs::file_size(pathA, ec);
    if (ec) {
        return false;
    }
    const auto sizeB = fs::file_size(pathB, ec);
    if (ec || sizeA != sizeB) {
        return false;
    }
    std::ifstream inA(pathA, std::ios::binary);
    std::ifstream inB(pathB, std::ios::binary);
    if (!inA || !inB) {
        return false;
    }
    std::vector<char> bufferA(1 << 20);
    std::vector<char> bufferB(1 << 20);
    std::uintmax_t compared = 0;
    while (compared < sizeA) {
        const auto chunk = static_cast<std::streamsize>(
            std::min<std::uintmax_t>(bufferA.size(), sizeA - compared));
        inA.read(bufferA.data(), chunk);
        inB.read(bufferB.data(), chunk);
        // A short read is a file that changed under us or a stick that
        // stopped answering: not the same, whatever was read so far.
        if (inA.gcount() != chunk || inB.gcount() != chunk) {
            return false;
        }
        if (!std::equal(bufferA.begin(), bufferA.begin() + chunk, bufferB.begin())) {
            return false;
        }
        compared += static_cast<std::uintmax_t>(chunk);
    }
    return true;
}

bool IdenticalFileAlignmentProbe::sameContent(const std::string &absolutePathA, const std::string &absolutePathB)
{
    if (absolutePathA != m_lastA || absolutePathB != m_lastB) {
        m_lastA = absolutePathA;
        m_lastB = absolutePathB;
        m_lastSame = filesHaveSameBytes(absolutePathA, absolutePathB);
        if (m_lastSame) {
            ++m_identical;
        }
    }
    return m_lastSame;
}

domain::AudioAlignmentProbe::Answer IdenticalFileAlignmentProbe::align(const std::string &absolutePathA,
                                                                       const std::string &absolutePathB)
{
    if (sameContent(absolutePathA, absolutePathB)) {
        return Answer::linedUp(0.0);
    }
    const Answer answer = m_decoder != nullptr ? m_decoder->align(absolutePathA, absolutePathB) : Answer::noOpinion();
    ++(answer.kind == Answer::Kind::LinedUp ? m_measured : m_unknown);
    return answer;
}

}  // namespace seabass::infrastructure::audio
