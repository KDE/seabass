// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_settings_fields.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace
{

// The category names, once, so a typo in the table below is a compile
// error rather than a group of one.
const std::string PlayerDjSetting = "Player: DJ setting";
const std::string PlayerDeckState = "Player: deck state";
const std::string PlayerScreen = "Player: screen";
const std::string PlayerLights = "Player: lights";
const std::string MixerFaders = "Mixer: faders";
const std::string MixerHeadphonesMic = "Mixer: headphones and microphone";
const std::string MixerEffectsMidi = "Mixer: effects and MIDI";
const std::string MixerLights = "Mixer: lights";

}  // namespace

const std::vector<std::string> &settingsCategoryOrder()
{
    static const std::vector<std::string> order = {
        PlayerDjSetting, PlayerDeckState, PlayerScreen, PlayerLights,
        MixerFaders,     MixerHeadphonesMic, MixerEffectsMidi, MixerLights,
    };
    return order;
}

const std::vector<SettingsFieldDescriptor> &allSettingsFields()
{
    static const std::vector<SettingsFieldDescriptor> fields = {
        // MYSETTING.DAT
        {"MYSETTING.DAT", "On-air display", 8, {{0x80, "off"}, {0x81, "on"}}, PlayerLights,
         "Whether the player shows that its channel is on air, when it is connected over PRO DJ LINK to a "
         "mixer that supports On Air Display."},
        {"MYSETTING.DAT",
         "LCD brightness",
         9,
         {{0x81, "1"}, {0x82, "2"}, {0x83, "3"}, {0x84, "4"}, {0x85, "5"}},
         PlayerScreen,
         "Brightness of the player's main screen."},
        {"MYSETTING.DAT", "Quantize", 10, {{0x80, "off"}, {0x81, "on"}}, PlayerDeckState,
         "Whether the Quantize button starts out on. With it on, cues, loops and beat jumps land on the beat, "
         "in steps of the quantize beat value. It moves when a cue fires, never where the cue is stored, and "
         "it can only be as right as the track's beat grid: a cue sitting more than half a step off snaps to "
         "the neighbouring beat rather than back to the one you meant."},
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
          {0x88, "memory"}},
         PlayerDjSetting,
         "How quiet the audio has to be for Auto cue to treat it as silence when it finds where a track starts. "
         "\"memory\" uses the stored cue nearest the start of the track instead."},
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
          {0x92, "Turkish"}},
         PlayerScreen,
         "The language of the player's screen."},
        {"MYSETTING.DAT", "Jog ring brightness", 14, {{0x80, "off"}, {0x81, "dark"}, {0x82, "bright"}},
         PlayerLights, "Brightness of the light ring around the jog wheel."},
        {"MYSETTING.DAT", "Jog ring indicator", 15, {{0x80, "off"}, {0x81, "on"}}, PlayerLights,
         "Whether the jog ring flashes when the playing track is close to its end."},
        {"MYSETTING.DAT", "Slip flashing", 16, {{0x80, "off"}, {0x81, "on"}}, PlayerLights,
         "Whether the buttons that work with Slip flash while Slip is switched on."},
        {"MYSETTING.DAT", "Disc slot illumination", 20, {{0x80, "off"}, {0x81, "dark"}, {0x82, "bright"}},
         PlayerLights, "Brightness of the light around the disc slot, on players that have one."},
        {"MYSETTING.DAT", "Eject lock", 21, {{0x80, "unlock"}, {0x81, "lock"}}, PlayerDjSetting,
         "Whether a different track can be loaded, or media ejected, while a track is playing."},
        {"MYSETTING.DAT", "Sync", 22, {{0x80, "off"}, {0x81, "on"}}, PlayerDeckState,
         "Whether Beat Sync starts out on."},
        {"MYSETTING.DAT", "Play mode", 23, {{0x80, "continue"}, {0x81, "single"}}, PlayerDjSetting,
         "What happens at the end of a track: \"continue\" loads the next one automatically, \"single\" stops."},
        {"MYSETTING.DAT",
         "Quantize beat value",
         24,
         {{0x80, "1"}, {0x81, "1/2"}, {0x82, "1/4"}, {0x83, "1/8"}},
         PlayerDjSetting,
         "The size of the step Quantize snaps to, in beats. A smaller step moves a slightly-off cue less, and "
         "rescues a badly-off one less too: anything past half a step lands on the next beat instead."},
        {"MYSETTING.DAT",
         "Hot cue autoload",
         25,
         {{0x80, "off"}, {0x81, "on"}, {0x82, "rekordbox setting"}},
         PlayerDjSetting,
         "Whether a track's hot cues are called up when it is loaded. \"rekordbox setting\" follows the choice "
         "made for each track in rekordbox."},
        {"MYSETTING.DAT", "Hot cue color", 26, {{0x80, "off"}, {0x81, "on"}}, PlayerLights,
         "Whether the hot cue pads (A to H) light up in each cue's own colour. This does not choose the "
         "colours: those belong to the cues themselves."},
        {"MYSETTING.DAT", "Needle lock", 29, {{0x80, "unlock"}, {0x81, "lock"}}, PlayerDjSetting,
         "Whether touching the track overview jumps to that point while a track is playing."},
        {"MYSETTING.DAT", "Time mode", 32, {{0x80, "elapsed"}, {0x81, "remaining"}}, PlayerDeckState,
         "Whether the time display counts the time played or the time left."},
        {"MYSETTING.DAT", "Jog mode", 33, {{0x80, "CDJ"}, {0x81, "vinyl"}}, PlayerDeckState,
         "How the jog wheel behaves. \"vinyl\" stops playback when you press its top and scratches when you "
         "press and turn it; \"CDJ\" does neither."},
        {"MYSETTING.DAT", "Auto cue", 34, {{0x80, "off"}, {0x81, "on"}}, PlayerDeckState,
         "Whether a cue is set automatically where the audio starts when a track is loaded, using the auto "
         "cue level."},
        {"MYSETTING.DAT", "Master tempo", 35, {{0x80, "off"}, {0x81, "on"}}, PlayerDeckState,
         "Whether changing the tempo leaves the pitch where it is."},
        {"MYSETTING.DAT",
         "Tempo range",
         36,
         {{0x80, "±6%"}, {0x81, "±10%"}, {0x82, "±16%"}, {0x83, "wide"}},
         PlayerDeckState,
         "How far the tempo slider can move the tempo."},
        {"MYSETTING.DAT", "Phase meter", 37, {{0x80, "type 1"}, {0x81, "type 2"}}, PlayerScreen,
         "Which of the player's two phase meter styles is drawn."},

        // MYSETTING2.DAT
        {"MYSETTING2.DAT",
         "Vinyl speed adjust",
         0,
         {{0x80, "touch & release"}, {0x81, "touch"}, {0x82, "release"}},
         PlayerDjSetting,
         "In vinyl mode, whether stopping (touch), starting again (release) or both slow down like a "
         "turntable instead of happening at once."},
        {"MYSETTING2.DAT",
         "Jog display mode",
         1,
         {{0x80, "auto"}, {0x81, "info"}, {0x82, "simple"}, {0x83, "artwork"}},
         PlayerScreen,
         "What the display in the middle of the jog wheel shows."},
        {"MYSETTING2.DAT",
         "Pad button brightness",
         2,
         {{0x81, "1"}, {0x82, "2"}, {0x83, "3"}, {0x84, "4"}},
         PlayerLights,
         "Brightness of the performance pads."},
        {"MYSETTING2.DAT",
         "Jog LCD brightness",
         3,
         {{0x81, "1"}, {0x82, "2"}, {0x83, "3"}, {0x84, "4"}, {0x85, "5"}},
         PlayerScreen,
         "Brightness of the display in the middle of the jog wheel."},
        {"MYSETTING2.DAT", "Waveform divisions", 4, {{0x80, "time scale"}, {0x81, "phrase"}}, PlayerScreen,
         "Whether the markings on the waveform divide it by time or by phrase."},
        {"MYSETTING2.DAT", "Waveform", 10, {{0x80, "waveform"}, {0x81, "phase meter"}}, PlayerScreen,
         "Whether the playback screen shows the waveform or the phase meter."},
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
          {0x87, "64"}},
         PlayerDjSetting,
         "How many beats the Beat Jump buttons move."},

        // DJMMYSETTING.DAT
        {"DJMMYSETTING.DAT",
         "Channel fader curve",
         12,
         {{0x80, "steep top"}, {0x81, "linear"}, {0x82, "steep bottom"}},
         MixerFaders,
         "How quickly a channel comes in as its fader moves up: sharply near the top, evenly, or sharply near "
         "the bottom."},
        {"DJMMYSETTING.DAT",
         "Crossfader curve",
         13,
         {{0x80, "constant"}, {0x81, "slow cut"}, {0x82, "fast cut"}},
         MixerFaders,
         "How the crossfader moves from A to B: gradually, or cutting the other side in almost immediately."},
        {"DJMMYSETTING.DAT", "Headphones pre EQ", 14, {{0x80, "post EQ"}, {0x81, "pre EQ"}}, MixerHeadphonesMic,
         "Whether what you cue in the headphones is heard before or after the channel's EQ."},
        {"DJMMYSETTING.DAT", "Headphones mono split", 15, {{0x80, "stereo"}, {0x81, "mono split"}},
         MixerHeadphonesMic,
         "\"mono split\" puts the cued channels in the left ear and the master in the right; \"stereo\" plays "
         "the cued channels in both."},
        {"DJMMYSETTING.DAT", "Beat FX quantize", 16, {{0x80, "off"}, {0x81, "on"}}, MixerEffectsMidi,
         "Whether Beat FX lock to the beat."},
        {"DJMMYSETTING.DAT", "Mic low cut", 17, {{0x80, "off"}, {0x81, "on"}}, MixerHeadphonesMic,
         "Cuts the low frequencies out of the microphone."},
        {"DJMMYSETTING.DAT", "Talk over mode", 18, {{0x80, "advanced"}, {0x81, "normal"}}, MixerHeadphonesMic,
         "What talk over turns down while you speak: \"advanced\" lowers only the mid-range of the other "
         "channels, \"normal\" lowers them entirely."},
        {"DJMMYSETTING.DAT",
         "Talk over level",
         19,
         {{0x80, "-24dB"}, {0x81, "-18dB"}, {0x82, "-12dB"}, {0x83, "-6dB"}},
         MixerHeadphonesMic,
         "How far talk over turns the other channels down."},
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
          {0x8F, "16"}},
         MixerEffectsMidi,
         "The MIDI channel the mixer sends on."},
        {"DJMMYSETTING.DAT", "MIDI button type", 21, {{0x80, "toggle"}, {0x81, "trigger"}}, MixerEffectsMidi,
         "Whether the mixer's buttons send MIDI as toggles or as triggers."},
        {"DJMMYSETTING.DAT",
         "Display brightness",
         22,
         {{0x80, "white"}, {0x81, "1"}, {0x82, "2"}, {0x83, "3"}, {0x84, "4"}, {0x85, "5"}},
         MixerLights,
         "Brightness of the mixer's display."},
        {"DJMMYSETTING.DAT", "Indicator brightness", 23, {{0x80, "1"}, {0x81, "2"}, {0x82, "3"}}, MixerLights,
         "Brightness of the mixer's indicator lights."},
        {"DJMMYSETTING.DAT",
         "Channel fader curve (long)",
         24,
         {{0x80, "exponential"}, {0x81, "smooth"}, {0x82, "linear"}},
         MixerFaders,
         "The channel fader curve on mixers with long channel faders."},
    };
    return fields;
}

bool isOffOnSwitch(const std::vector<SettingsFieldOption> &options, const std::string &currentValue)
{
    return options.size() == 2 && options[0].name == "off" && options[1].name == "on"
        && (currentValue == "off" || currentValue == "on");
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
