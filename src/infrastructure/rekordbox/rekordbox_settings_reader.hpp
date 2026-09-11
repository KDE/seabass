// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace seabass::infrastructure::rekordbox
{

// One decoded settings file: player/mixer preferences as human-readable
// label/value pairs, in field order.
struct SettingsFile
{
    std::string fileName;
    std::string title;
    std::vector<std::pair<std::string, std::string>> fields;
};

// Reads whichever of MYSETTING.DAT / MYSETTING2.DAT / DJMMYSETTING.DAT are
// present directly under pioneerRoot (device/player preferences such as
// auto cue level, language, jog mode, mixer curves, ...). Best-effort:
// a missing or unexpectedly-sized file is skipped rather than failing the
// whole read.
//
// Byte layout cross-checked against a real captured MYSETTING.DAT (exact
// file size, exact header strings, and several plausible decoded field
// values all matched) and against pyrekordbox's mysettings module
// (MIT-licensed, https://github.com/dylanljones/pyrekordbox), which
// itself credits Jan Holthuis's rekordcrate for the format. DEVSETTING.DAT
// is deliberately not decoded -- even pyrekordbox can't parse its data,
// only its header.
std::vector<SettingsFile> readDeviceSettings(const std::string &pioneerRoot);

}  // namespace seabass::infrastructure::rekordbox
