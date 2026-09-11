// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace seabass::infrastructure::rekordbox
{

// One valid byte value for a settings field, with its human-readable name
// (e.g. {0x81, "on"}).
struct SettingsFieldOption
{
    uint8_t value;
    std::string name;
};

// One field within a settings file's data section. byteOffset is relative
// to the start of the data section (i.e. HeaderSize bytes into the file --
// see rekordbox_settings_reader.cpp), matching readDeviceSettings()'s
// existing per-byte offsets exactly.
struct SettingsFieldDescriptor
{
    std::string fileName;  // "MYSETTING.DAT", "MYSETTING2.DAT", or "DJMMYSETTING.DAT"
    std::string label;
    size_t byteOffset;
    std::vector<SettingsFieldOption> options;
};

// The single source of truth for every known settings field -- shared by
// the reader (decodes the current byte against `options`) and the writer
// (looks up the byte for a chosen option name), so the two can never drift
// apart. Byte layout cross-checked against a real captured MYSETTING.DAT
// and pyrekordbox's mysettings module (MIT-licensed,
// https://github.com/dylanljones/pyrekordbox).
const std::vector<SettingsFieldDescriptor> &allSettingsFields();

// Header is always len_strings(4) + brand(32) + software(32) + version(32)
// + len_data(4) = 104 bytes, regardless of file type (the strings are
// fixed-width, padded with zeros). Footer is checksum(2) + unknown(2).
constexpr size_t SettingsHeaderSize = 104;
constexpr size_t SettingsFooterSize = 4;

// Size of the data section (between header and footer) for a given
// settings file name.
size_t settingsDataSizeFor(const std::string &fileName);

}  // namespace seabass::infrastructure::rekordbox

namespace seabass::infrastructure::rekordbox::detail
{
// CRC16/XMODEM (poly 0x1021, init 0x0000, no reflection, no final xor) --
// matches pyrekordbox's compute_checksum() exactly. Exposed here (not just
// as a writer implementation detail) so it can be unit-tested directly
// against known-good values.
uint16_t crc16Xmodem(const uint8_t *data, size_t length);

}  // namespace seabass::infrastructure::rekordbox::detail
