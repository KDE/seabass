// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// A strict look at an ANLZ file after a cue write, for tests.
//
// Deliberately NOT the production code's own checks: a writer verified
// by the rules it was written with agrees with itself whatever it does.
// This is the kaitai parser the reader is built on, with no tolerance,
// plus the invariants of the survey in anlz_legacy_cue_codec.hpp that
// kaitai does not check (it reads a fixed 56 bytes per entry whatever
// len_entry says, and never compares len_tag with num_cues).

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <kaitai/kaitaistream.h>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"

namespace anlz_strict
{

namespace fs = std::filesystem;
using Anlz = rekordbox_anlz_t;
using seabass::pathToUtf8;

inline std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline uint32_t be32(const std::string &s, size_t at)
{
    return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) | (uint32_t(uint8_t(s[at + 2])) << 8)
        | uint32_t(uint8_t(s[at + 3]));
}

inline uint16_t be16(const std::string &s, size_t at)
{
    return uint16_t((uint16_t(uint8_t(s[at])) << 8) | uint8_t(s[at + 1]));
}

inline std::string hexDump(const std::string &bytes, size_t limit = 96)
{
    std::ostringstream out;
    const size_t n = std::min(bytes.size(), limit);
    for (size_t i = 0; i < n; ++i) {
        if (i % 16 == 0) {
            out << "\n        " << std::setw(4) << std::setfill('0') << std::hex << i << ":";
        }
        out << ' ' << std::setw(2) << std::setfill('0') << std::hex << unsigned(uint8_t(bytes[i]));
    }
    return out.str();
}

struct Section
{
    size_t offset = 0;
    std::string fourcc;
    std::string bytes;
};

// The file's sections as they sit on disk, without judging them.
inline std::vector<Section> sectionsOf(const std::string &data)
{
    std::vector<Section> out;
    if (data.size() < 12 || data.compare(0, 4, "PMAI") != 0) {
        return out;
    }
    size_t pos = be32(data, 4);
    while (pos + 12 <= data.size()) {
        const uint32_t lenTag = be32(data, pos + 8);
        if (lenTag < 12 || pos + lenTag > data.size()) {
            break;
        }
        out.push_back({pos, data.substr(pos, 4), data.substr(pos, lenTag)});
        pos += lenTag;
    }
    return out;
}

// What a legacy list of one type holds in a file, or that it has none.
enum class ListState { Absent, Empty, Populated };

struct LegacyList
{
    ListState state = ListState::Absent;
    std::string bytes;
};

inline LegacyList legacyList(const std::string &data, uint32_t listType)
{
    for (const auto &section : sectionsOf(data)) {
        if (section.fourcc == "PCOB" && section.bytes.size() >= 24 && be32(section.bytes, 12) == listType) {
            return {be16(section.bytes, 18) == 0 ? ListState::Empty : ListState::Populated, section.bytes};
        }
    }
    return {};
}

// Everything wrong with one file after a write, each problem naming the
// section and showing its bytes. Empty means it is well formed.
inline std::vector<std::string> strictProblems(const fs::path &path)
{
    std::vector<std::string> problems;
    const std::string data = readFile(path);
    const std::string where = pathToUtf8(path);

    // The survey's invariants, which kaitai does not check itself: it
    // reads a fixed 56 bytes per entry whatever len_entry says, and never
    // compares len_tag with num_cues.
    size_t walked = data.size() >= 8 ? be32(data, 4) : 0;
    for (const auto &section : sectionsOf(data)) {
        walked = section.offset + section.bytes.size();
        if (section.fourcc != "PCOB") {
            continue;
        }
        const std::string &s = section.bytes;
        auto fail = [&](const std::string &what) {
            std::ostringstream msg;
            msg << where << ": PCOB at offset " << section.offset << ": " << what << hexDump(s);
            problems.push_back(msg.str());
        };
        if (s.size() < 24 || be32(s, 4) != 24) {
            fail("len_header is not 24");
            continue;
        }
        const uint16_t numCues = be16(s, 18);
        if (be32(s, 8) != 24u + 56u * numCues) {
            fail("len_tag " + std::to_string(be32(s, 8)) + " is not 24 + 56 * " + std::to_string(numCues));
            continue;
        }
        if (be16(s, 16) != 0) {
            fail("the two bytes before num_cues are not zero");
        }
        for (uint16_t i = 0; i < numCues; ++i) {
            const size_t at = 24 + size_t(56) * i;
            if (s.compare(at, 4, "PCPT") != 0 || be32(s, at + 4) != 28 || be32(s, at + 8) != 56) {
                fail("entry " + std::to_string(i) + " is not a 56-byte PCPT entry with len_header 28");
                break;
            }
        }
    }
    if (walked != data.size()) {
        problems.push_back(where + ": sections do not tile the file (stopped at " + std::to_string(walked) + " of "
                           + std::to_string(data.size()) + ")");
    }

    // And the parser itself, with no tolerance at all.
    try {
        std::istringstream in(data, std::ios::binary);
        kaitai::kstream ks(&in);
        Anlz anlz(&ks);
        for (const auto &section : *anlz.sections()) {
            if (section->fourcc() != Anlz::SECTION_TAGS_CUES) {
                continue;
            }
            auto *tag = dynamic_cast<Anlz::cue_tag_t *>(section->body());
            if (!tag || tag->cues()->size() != tag->num_cues()) {
                problems.push_back(where + ": kaitai read a PCOB whose entries do not match num_cues");
            }
        }
    } catch (const std::exception &e) {
        problems.push_back(where + ": kaitai could not parse it: " + e.what());
    }
    return problems;
}

// slot -> position of each hot cue in a file's legacy hot list, read
// through kaitai (strict).
inline std::multimap<uint32_t, uint32_t> legacyHotSlots(const fs::path &path)
{
    std::multimap<uint32_t, uint32_t> out;
    const std::string data = readFile(path);
    std::istringstream in(data, std::ios::binary);
    kaitai::kstream ks(&in);
    Anlz anlz(&ks);
    for (const auto &section : *anlz.sections()) {
        if (section->fourcc() != Anlz::SECTION_TAGS_CUES) {
            continue;
        }
        auto *tag = dynamic_cast<Anlz::cue_tag_t *>(section->body());
        if (!tag || tag->type() != Anlz::CUE_LIST_TYPE_HOT_CUES) {
            continue;
        }
        for (const auto &cue : *tag->cues()) {
            out.emplace(cue->hot_cue(), cue->time());
        }
    }
    return out;
}

}  // namespace anlz_strict
