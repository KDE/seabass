// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_settings_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>

#include "infrastructure/rekordbox/rekordbox_settings_fields.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace
{

std::string decodeField(uint8_t byteValue, const std::vector<SettingsFieldOption> &options)
{
    for (const auto &option : options) {
        if (option.value == byteValue) {
            return option.name;
        }
    }
    return "unknown (0x" + std::to_string(byteValue) + ")";
}

// Reads one settings file if it exists and is the expected size, decoding
// every known field for fileName via the shared descriptor table. Returns
// nullopt (not an exception) for a missing or unexpectedly-sized file --
// this is supplementary display data, never worth failing over.
std::optional<SettingsFile> readOne(const std::string &pioneerRoot, const std::string &fileName,
                                     const std::string &title)
{
    std::ifstream ifs(pioneerRoot + "/" + fileName, std::ifstream::binary);
    if (!ifs.is_open()) {
        return std::nullopt;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (data.size() != SettingsHeaderSize + settingsDataSizeFor(fileName) + SettingsFooterSize) {
        return std::nullopt;
    }

    SettingsFile result;
    result.fileName = fileName;
    result.title = title;
    for (const auto &field : allSettingsFields()) {
        if (field.fileName != fileName) {
            continue;
        }
        result.fields.emplace_back(field.label,
                                    decodeField(data[SettingsHeaderSize + field.byteOffset], field.options));
    }
    return result;
}

}  // namespace

std::vector<SettingsFile> readDeviceSettings(const std::string &pioneerRoot)
{
    std::vector<SettingsFile> results;

    if (auto f = readOne(pioneerRoot, "MYSETTING.DAT", "Player settings")) {
        results.push_back(std::move(*f));
    }
    if (auto f = readOne(pioneerRoot, "MYSETTING2.DAT", "Player settings (2)")) {
        results.push_back(std::move(*f));
    }
    if (auto f = readOne(pioneerRoot, "DJMMYSETTING.DAT", "Mixer settings")) {
        results.push_back(std::move(*f));
    }

    return results;
}

}  // namespace seabass::infrastructure::rekordbox
