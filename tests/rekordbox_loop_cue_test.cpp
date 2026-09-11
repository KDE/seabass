// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"

#include "scratch_path.hpp"

using Anlz = rekordbox_anlz_t;
namespace fs = std::filesystem;
using namespace seabass::infrastructure::rekordbox;

namespace
{

std::string fromHex(const std::string &hex)
{
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

void writeFile(const fs::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace

// Confirms the kaitai-generated ANLZ parser exposes exactly the fields
// kaitai_rekordbox_reader.cpp's readCues() reads to populate
// CuePoint::isLoop/loopEndMs (cue->type()/cue->loop_time()) -- against a
// real captured loop entry, the same way anlz_cue_codec_test.cpp
// exercises AnlzCueCodec with real hot-cue captures rather than only
// synthetic ones.
int main()
{
    // Real PCO2 (memory-cue list) section captured from
    // testdata/PIONEER-rb7-stick's USBANLZ/P017/0002435D/ANLZ0000.EXT --
    // the one real loop cue found anywhere in this project's test data,
    // located by scanning every ANLZ file this repo has access to for
    // cue_entry_type == 2. A memory-list loop (hot_cue=0), loop-in
    // 52583ms, loop-out 56333ms.
    std::string section = fromHex(
        "50434f32000000140000006c0000000000010000504350320000001000000058000000000200"
        "03e80000cd670000dc0d00010246020200000008000100000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000");

    // Wrap in a minimal-but-structurally-real ANLZ file (same shape
    // anlz_file_test.cpp's buildMinimalAnlz() uses: 12-byte header, one
    // section) so the real kaitai_anlz_t parser -- not a hand-rolled
    // stand-in -- does the actual field decoding.
    std::string data = "PMAI";
    appendU32BE(data, 12);  // len_header
    appendU32BE(data, 0);   // len_file, patched below
    data += section;
    writeU32BE(data, 8, static_cast<uint32_t>(data.size()));

    fs::path tmp = seabass::testing::scratchRoot() / "seabass_rekordbox_loop_cue_test.anlz";
    writeFile(tmp, data);

    std::ifstream ifs(tmp, std::ifstream::binary);
    assert(ifs.is_open());
    kaitai::kstream ks(&ifs);
    Anlz anlz(&ks);

    bool found = false;
    for (const auto &section_ : *anlz.sections()) {
        if (section_->fourcc() != Anlz::SECTION_TAGS_CUES_2) {
            continue;
        }
        auto *tag = dynamic_cast<Anlz::cue_extended_tag_t *>(section_->body());
        assert(tag != nullptr);
        assert(tag->type() == Anlz::CUE_LIST_TYPE_MEMORY_CUES);
        for (const auto &cue : *tag->cues()) {
            assert(cue->hot_cue() == 0);
            assert(cue->type() == Anlz::CUE_ENTRY_TYPE_LOOP);
            assert(cue->time() == 52583);
            assert(cue->loop_time() == 56333);
            found = true;
        }
    }
    assert(found);

    std::error_code ec;
    fs::remove(tmp, ec);

    std::cout << "case 1 (real memory-list loop cue: type=loop, loop_time set) OK\n";
    return 0;
}
