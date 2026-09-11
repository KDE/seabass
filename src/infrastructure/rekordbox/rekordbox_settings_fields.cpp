// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_settings_fields.hpp"

namespace seabass::infrastructure::rekordbox
{

const std::vector<SettingsFieldDescriptor> &allSettingsFields()
{
    static const std::vector<SettingsFieldDescriptor> fields = {
        // MYSETTING.DAT
        {"MYSETTING.DAT", "On-air display", 8, {{0x80, "off"}, {0x81, "on"}}},
        {"MYSETTING.DAT",
         "LCD brightness",
         9,
         {{0x81, "1"}, {0x82, "2"}, {0x83, "3"}, {0x84, "4"}, {0x85, "5"}}},
        {"MYSETTING.DAT", "Quantize", 10, {{0x80, "off"}, {0x81, "on"}}},
        {"MYSETTING.DAT",
         "Auto cue level",
         11,
         {{0x80, "-36dB"},
          {0x81, "-42dB"},
          {0x82, "-48dB"},
          {0x83, "-54dB"},
          {0x84, "-60dB"},
          {0x85, "-66dB"},
          {0x86, "-72dB"},
          {0x87, "-78dB"},
          {0x88, "memory"}}},
        {"MYSETTING.DAT",
         "Language",
         12,
         {{0x81, "English"},
          {0x82, "French"},
          {0x83, "German"},
          {0x84, "Italian"},
          {0x85, "Dutch"},
          {0x86, "Spanish"},
          {0x87, "Russian"},
          {0x88, "Korean"},
          {0x89, "Chinese (simplified)"},
          {0x8A, "Chinese (traditional)"},
          {0x8B, "Japanese"},
          {0x8C, "Portuguese"},
          {0x8D, "Swedish"},
          {0x8E, "Czech"},
          {0x8F, "Hungarian"},
          {0x90, "Danish"},
          {0x91, "Greek"},
          {0x92, "Turkish"}}},
        {"MYSETTING.DAT", "Jog ring brightness", 14, {{0x80, "off"}, {0x81, "dark"}, {0x82, "bright"}}},
        {"MYSETTING.DAT", "Jog ring indicator", 15, {{0x80, "off"}, {0x81, "on"}}},
        {"MYSETTING.DAT", "Slip flashing", 16, {{0x80, "off"}, {0x81, "on"}}},
        {"MYSETTING.DAT", "Disc slot illumination", 20, {{0x80, "off"}, {0x81, "dark"}, {0x82, "bright"}}},
        {"MYSETTING.DAT", "Eject lock", 21, {{0x80, "unlock"}, {0x81, "lock"}}},
        {"MYSETTING.DAT", "Sync", 22, {{0x80, "off"}, {0x81, "on"}}},
        {"MYSETTING.DAT", "Play mode", 23, {{0x80, "continue"}, {0x81, "single"}}},
        {"MYSETTING.DAT",
         "Quantize beat value",
         24,
         {{0x80, "1"}, {0x81, "1/2"}, {0x82, "1/4"}, {0x83, "1/8"}}},
        {"MYSETTING.DAT",
         "Hot cue autoload",
         25,
         {{0x80, "off"}, {0x81, "on"}, {0x82, "rekordbox setting"}}},
        {"MYSETTING.DAT", "Hot cue color", 26, {{0x80, "off"}, {0x81, "on"}}},
        {"MYSETTING.DAT", "Needle lock", 29, {{0x80, "unlock"}, {0x81, "lock"}}},
        {"MYSETTING.DAT", "Time mode", 32, {{0x80, "elapsed"}, {0x81, "remaining"}}},
        {"MYSETTING.DAT", "Jog mode", 33, {{0x80, "CDJ"}, {0x81, "vinyl"}}},
        {"MYSETTING.DAT", "Auto cue", 34, {{0x80, "off"}, {0x81, "on"}}},
        {"MYSETTING.DAT", "Master tempo", 35, {{0x80, "off"}, {0x81, "on"}}},
        {"MYSETTING.DAT",
         "Tempo range",
         36,
         {{0x80, "±6%"}, {0x81, "±10%"}, {0x82, "±16%"}, {0x83, "wide"}}},
        {"MYSETTING.DAT", "Phase meter", 37, {{0x80, "type 1"}, {0x81, "type 2"}}},

        // MYSETTING2.DAT
        {"MYSETTING2.DAT",
         "Vinyl speed adjust",
         0,
         {{0x80, "touch & release"}, {0x81, "touch"}, {0x82, "release"}}},
        {"MYSETTING2.DAT",
         "Jog display mode",
         1,
         {{0x80, "auto"}, {0x81, "info"}, {0x82, "simple"}, {0x83, "artwork"}}},
        {"MYSETTING2.DAT",
         "Pad button brightness",
         2,
         {{0x81, "1"}, {0x82, "2"}, {0x83, "3"}, {0x84, "4"}}},
        {"MYSETTING2.DAT",
         "Jog LCD brightness",
         3,
         {{0x81, "1"}, {0x82, "2"}, {0x83, "3"}, {0x84, "4"}, {0x85, "5"}}},
        {"MYSETTING2.DAT", "Waveform divisions", 4, {{0x80, "time scale"}, {0x81, "phrase"}}},
        {"MYSETTING2.DAT", "Waveform", 10, {{0x80, "waveform"}, {0x81, "phase meter"}}},
        {"MYSETTING2.DAT",
         "Beat jump beat value",
         12,
         {{0x80, "1/2"},
          {0x81, "1"},
          {0x82, "2"},
          {0x83, "4"},
          {0x84, "8"},
          {0x85, "16"},
          {0x86, "32"},
          {0x87, "64"}}},

        // DJMMYSETTING.DAT
        {"DJMMYSETTING.DAT",
         "Channel fader curve",
         12,
         {{0x80, "steep top"}, {0x81, "linear"}, {0x82, "steep bottom"}}},
        {"DJMMYSETTING.DAT",
         "Crossfader curve",
         13,
         {{0x80, "constant"}, {0x81, "slow cut"}, {0x82, "fast cut"}}},
        {"DJMMYSETTING.DAT", "Headphones pre EQ", 14, {{0x80, "post EQ"}, {0x81, "pre EQ"}}},
        {"DJMMYSETTING.DAT", "Headphones mono split", 15, {{0x80, "stereo"}, {0x81, "mono split"}}},
        {"DJMMYSETTING.DAT", "Beat FX quantize", 16, {{0x80, "off"}, {0x81, "on"}}},
        {"DJMMYSETTING.DAT", "Mic low cut", 17, {{0x80, "off"}, {0x81, "on"}}},
        {"DJMMYSETTING.DAT", "Talk over mode", 18, {{0x80, "advanced"}, {0x81, "normal"}}},
        {"DJMMYSETTING.DAT",
         "Talk over level",
         19,
         {{0x80, "-24dB"}, {0x81, "-18dB"}, {0x82, "-12dB"}, {0x83, "-6dB"}}},
        {"DJMMYSETTING.DAT",
         "MIDI channel",
         20,
         {{0x80, "1"},
          {0x81, "2"},
          {0x82, "3"},
          {0x83, "4"},
          {0x84, "5"},
          {0x85, "6"},
          {0x86, "7"},
          {0x87, "8"},
          {0x88, "9"},
          {0x89, "10"},
          {0x8A, "11"},
          {0x8B, "12"},
          {0x8C, "13"},
          {0x8D, "14"},
          {0x8E, "15"},
          {0x8F, "16"}}},
        {"DJMMYSETTING.DAT", "MIDI button type", 21, {{0x80, "toggle"}, {0x81, "trigger"}}},
        {"DJMMYSETTING.DAT",
         "Display brightness",
         22,
         {{0x80, "white"}, {0x81, "1"}, {0x82, "2"}, {0x83, "3"}, {0x84, "4"}, {0x85, "5"}}},
        {"DJMMYSETTING.DAT", "Indicator brightness", 23, {{0x80, "1"}, {0x81, "2"}, {0x82, "3"}}},
        {"DJMMYSETTING.DAT",
         "Channel fader curve (long)",
         24,
         {{0x80, "exponential"}, {0x81, "smooth"}, {0x82, "linear"}}},
    };
    return fields;
}

size_t settingsDataSizeFor(const std::string &fileName)
{
    return fileName == "DJMMYSETTING.DAT" ? 52 : 40;
}

}  // namespace seabass::infrastructure::rekordbox

namespace seabass::infrastructure::rekordbox::detail
{

uint16_t crc16Xmodem(const uint8_t *data, size_t length)
{
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

}  // namespace seabass::infrastructure::rekordbox::detail
