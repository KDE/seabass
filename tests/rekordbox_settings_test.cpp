// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "infrastructure/rekordbox/rekordbox_settings_fields.hpp"
#include "infrastructure/rekordbox/rekordbox_settings_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_settings_writer.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::rekordbox;

namespace
{

// A minimal but structurally valid MYSETTING.DAT: 104-byte header (only
// len_data at offset 100 matters here) + 40-byte data section (zeroed,
// which decodes every field as "unknown") + 4-byte footer (checksum +
// unknown, both zeroed).
std::vector<uint8_t> makeMySettingBytes()
{
    std::vector<uint8_t> data(104 + 40 + 4, 0);
    data[100] = 40;  // len_data, little-endian u32, value fits in one byte
    return data;
}

}  // namespace

int main()
{
    // CRC16/XMODEM known-answer test: CRC16-XMODEM("123456789") == 0x31C3
    // is the standard reference vector for this exact algorithm variant
    // (poly 0x1021, init 0x0000, no reflection, no final xor).
    {
        const char *check = "123456789";
        uint16_t crc = detail::crc16Xmodem(reinterpret_cast<const uint8_t *>(check), 9);
        assert(crc == 0x31C3);
        std::cout << "case 1 (CRC16/XMODEM known-answer test) OK\n";
    }

    // Writing a field, then reading it back, sees the new value -- and the
    // file still round-trips as valid (same size, checksum accepted).
    {
        std::filesystem::path dir = seabass::testing::scratchRoot() / "seabass_settings_test";
        std::filesystem::create_directories(dir);
        std::filesystem::path path = dir / "MYSETTING.DAT";
        {
            auto bytes = makeMySettingBytes();
            std::ofstream ofs(path, std::ofstream::binary | std::ofstream::trunc);
            ofs.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        bool ok = writeDeviceSettingField(dir.string(), "MYSETTING.DAT", "Auto cue level", "-60dB");
        assert(ok);

        auto files = readDeviceSettings(dir.string());
        assert(files.size() == 1);
        bool found = false;
        for (const auto &[label, value] : files[0].fields) {
            if (label == "Auto cue level") {
                assert(value == "-60dB");
                found = true;
            }
        }
        assert(found);
        std::cout << "case 2 (write then read back sees new value) OK\n";

        std::filesystem::remove_all(dir);
    }

    // Unknown field/option names are refused, not silently ignored.
    {
        std::filesystem::path dir = seabass::testing::scratchRoot() / "seabass_settings_test2";
        std::filesystem::create_directories(dir);
        std::filesystem::path path = dir / "MYSETTING.DAT";
        {
            auto bytes = makeMySettingBytes();
            std::ofstream ofs(path, std::ofstream::binary | std::ofstream::trunc);
            ofs.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        assert(!writeDeviceSettingField(dir.string(), "MYSETTING.DAT", "Not a real field", "on"));
        assert(!writeDeviceSettingField(dir.string(), "MYSETTING.DAT", "Auto cue level", "Not a real option"));
        std::cout << "case 3 (unknown field/option refused) OK\n";

        std::filesystem::remove_all(dir);
    }

    std::cout << "All rekordbox_settings_test cases passed.\n";
    return 0;
}
