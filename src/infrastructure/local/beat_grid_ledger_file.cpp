// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/beat_grid_ledger_file.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"

namespace seabass::infrastructure::local
{

namespace
{

namespace fs = std::filesystem;
using stick_backup::escapeManifestField;
using stick_backup::unescapeManifestField;

constexpr std::string_view Magic = "seabass-beat-grids";

// A ledger ten thousand tracks long is under a megabyte; this only stops
// a damaged file from taking the machine's memory with it.
constexpr std::uintmax_t MaxBytes = 64u * 1024u * 1024u;

bool isFingerprint(std::string_view text)
{
    if (text.size() != 64) {
        return false;
    }
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string header()
{
    return std::string(Magic) + "\t" + std::to_string(BeatGridLedgerFormatVersion) + "\n";
}

}  // namespace

std::string serializeBeatGridLedger(const BeatGridLedger &ledger)
{
    std::string text = header();
    for (const auto &[key, fingerprint] : ledger) {
        text += escapeManifestField(key);
        text += '\t';
        text += fingerprint;
        text += '\n';
    }
    return text;
}

BeatGridLedger readBeatGridLedger(const fs::path &stickRoot, std::string *error)
{
    if (!error) {
        throw std::invalid_argument("readBeatGridLedger: error is required");
    }
    error->clear();
    const fs::path file = paths::stickBeatGridLedger(stickRoot);
    const std::string name = pathToUtf8(file);
    const auto refuse = [&](const std::string &why) {
        *error = name + ": " + why;
        return BeatGridLedger{};
    };

    std::error_code ec;
    const bool exists = fs::exists(file, ec);
    if (ec) {
        return refuse("cannot be looked at (" + ec.message() + ")");
    }
    if (!exists) {
        return {};
    }
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        return refuse("cannot be read (" + ec.message() + ")");
    }
    if (size > MaxBytes) {
        return refuse("is " + std::to_string(size) + " bytes, far more than a ledger holds");
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return refuse("cannot be opened");
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return refuse("cannot be read");
    }
    if (text.empty() || text.back() != '\n') {
        return refuse("does not end with a newline: it was cut short");
    }

    BeatGridLedger ledger;
    std::size_t start = 0;
    int lineNumber = 0;
    std::optional<std::string> previousKey;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string_view line(text.data() + start, end - start);
        start = end + 1;
        ++lineNumber;
        const std::string where = "line " + std::to_string(lineNumber) + ": ";
        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos || line.find('\t', tab + 1) != std::string_view::npos) {
            return refuse(where + "not two fields");
        }
        const std::string_view first = line.substr(0, tab);
        const std::string_view second = line.substr(tab + 1);
        if (lineNumber == 1) {
            if (first != Magic) {
                return refuse("not a Seabass beat grid ledger");
            }
            if (second != std::to_string(BeatGridLedgerFormatVersion)) {
                return refuse("format version " + std::string(second) + ", this Seabass reads "
                              + std::to_string(BeatGridLedgerFormatVersion));
            }
            continue;
        }
        const std::optional<std::string> key = unescapeManifestField(first);
        if (!key || key->empty()) {
            return refuse(where + "no key, or a key with a bad escape");
        }
        if (!isFingerprint(second)) {
            return refuse(where + "the fingerprint is not 64 lowercase hex digits");
        }
        if (previousKey && !(*previousKey < *key)) {
            return refuse(where + (*previousKey == *key ? "a key listed twice" : "keys out of order"));
        }
        previousKey = *key;
        ledger.emplace(*key, std::string(second));
    }
    if (lineNumber == 0) {
        return refuse("no header");
    }
    return ledger;
}

bool writeBeatGridLedger(const fs::path &stickRoot, const BeatGridLedger &ledger, std::string *error)
{
    if (!error) {
        throw std::invalid_argument("writeBeatGridLedger: error is required");
    }
    error->clear();
    for (const auto &[key, fingerprint] : ledger) {
        if (key.empty()) {
            *error = "the beat grid ledger has an entry with no key";
            return false;
        }
        if (!isFingerprint(fingerprint)) {
            *error = "the beat grid ledger's entry for " + key + " has no proper fingerprint";
            return false;
        }
    }
    std::error_code ec;
    if (!fs::is_directory(stickRoot, ec)) {
        *error = pathToUtf8(stickRoot) + " is not a directory";
        return false;
    }
    const fs::path file = paths::stickBeatGridLedger(stickRoot);
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        *error = "cannot create " + pathToUtf8(file.parent_path()) + " (" + ec.message() + ")";
        return false;
    }
    if (!writeFileDurablyAtomic(pathToUtf8(file), serializeBeatGridLedger(ledger))) {
        *error = "cannot write " + pathToUtf8(file);
        return false;
    }
    return true;
}

}  // namespace seabass::infrastructure::local
