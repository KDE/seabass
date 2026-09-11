#include "infrastructure/rekordbox/rekordbox_settings_writer.hpp"

#include "infrastructure/durable_file_write.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>

#include "infrastructure/rekordbox/rekordbox_settings_fields.hpp"

namespace seabass::infrastructure::rekordbox
{

bool writeDeviceSettingField(const std::string &pioneerRoot, const std::string &fileName,
                              const std::string &fieldLabel, const std::string &optionName)
{
    const SettingsFieldDescriptor *field = nullptr;
    for (const auto &f : allSettingsFields()) {
        if (f.fileName == fileName && f.label == fieldLabel) {
            field = &f;
            break;
        }
    }
    if (!field) {
        return false;
    }

    const SettingsFieldOption *option = nullptr;
    for (const auto &o : field->options) {
        if (o.name == optionName) {
            option = &o;
            break;
        }
    }
    if (!option) {
        return false;
    }

    std::string path = pioneerRoot + "/" + fileName;
    std::vector<uint8_t> data;
    {
        std::ifstream ifs(path, std::ifstream::binary);
        if (!ifs.is_open()) {
            return false;
        }
        data.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }

    size_t expectedSize = SettingsHeaderSize + settingsDataSizeFor(fileName) + SettingsFooterSize;
    if (data.size() != expectedSize) {
        return false;  // not the layout this code understands -- refuse rather than guess
    }

    data[SettingsHeaderSize + field->byteOffset] = option->value;

    // MYSETTING.DAT/MYSETTING2.DAT checksum the data section only; DJMMYSETTING.DAT
    // checksums the whole file (including the header) -- both exclude the
    // trailing 4-byte footer itself. Matches pyrekordbox's compute_checksum().
    size_t checksumStart = fileName == "DJMMYSETTING.DAT" ? 0 : SettingsHeaderSize;
    size_t checksumLength = data.size() - SettingsFooterSize - checksumStart;
    uint16_t checksum = detail::crc16Xmodem(data.data() + checksumStart, checksumLength);

    // Little-endian; the trailing 2 "unknown" bytes are left exactly as
    // they were read, never forced to a guessed value.
    data[data.size() - 4] = static_cast<uint8_t>(checksum & 0xFF);
    data[data.size() - 3] = static_cast<uint8_t>((checksum >> 8) & 0xFF);

    // Temp file, fsync, rename: the one rekordbox write that used to go
    // through a truncating ofstream, which a stick pull between the
    // truncate and the write turned into a 0-byte settings file the
    // player then discarded.
    return writeFileDurablyAtomic(path, std::string(reinterpret_cast<const char *>(data.data()), data.size()));
}

}  // namespace seabass::infrastructure::rekordbox
