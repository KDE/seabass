#include "infrastructure/stick_backup/backup_manifest.hpp"

#include <charconv>
#include <string>
#include <vector>

namespace seabass::infrastructure::stick_backup
{

namespace
{

constexpr std::string_view Magic = "seabass-stick-manifest";
constexpr std::string_view TrailerPrefix = "#sha256\t";

std::vector<std::string_view> splitTabs(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        std::size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

template <typename T>
bool parseNumber(std::string_view text, T &out)
{
    if (text.empty()) {
        return false;
    }
    auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool fail(std::string *error, std::string message)
{
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

}  // namespace

std::string_view toString(BackupStatus status)
{
    switch (status) {
    case BackupStatus::Complete: return "complete";
    case BackupStatus::PartialCancelled: return "partial-cancelled";
    case BackupStatus::PartialConflict: return "partial-conflict";
    case BackupStatus::PartialDbTooLarge: return "partial-db-too-large";
    case BackupStatus::PartialSkipped: return "partial-skipped";
    }
    return "complete";
}

std::optional<BackupStatus> backupStatusFromString(std::string_view text)
{
    for (BackupStatus status : {BackupStatus::Complete, BackupStatus::PartialCancelled, BackupStatus::PartialConflict,
                                BackupStatus::PartialDbTooLarge, BackupStatus::PartialSkipped}) {
        if (toString(status) == text) {
            return status;
        }
    }
    return std::nullopt;
}

std::string escapeManifestField(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        switch (c) {
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\\': out += "\\\\"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

std::optional<std::string> unescapeManifestField(std::string_view escaped)
{
    std::string out;
    out.reserve(escaped.size());
    for (std::size_t i = 0; i < escaped.size(); ++i) {
        char c = escaped[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= escaped.size()) {
            return std::nullopt;
        }
        char next = escaped[++i];
        switch (next) {
        case 't': out.push_back('\t'); break;
        case 'n': out.push_back('\n'); break;
        case '\\': out.push_back('\\'); break;
        default: return std::nullopt;
        }
    }
    return out;
}

std::string BackupManifest::serialize() const
{
    std::string out;
    out += Magic;
    out += '\t';
    out += std::to_string(FormatVersion);
    out += '\t';
    out += escapeManifestField(stickIdentifier);
    out += '\t';
    out += escapeManifestField(stickLabel);
    out += '\t';
    out += toString(status);
    out += '\t';
    out += std::to_string(createdAtUnix);
    out += '\t';
    out += escapeManifestField(libraryFingerprint);
    out += '\n';

    for (const ManifestRow &row : rows) {
        out += row.kind == ManifestRow::Kind::Directory ? 'd' : 'f';
        out += '\t';
        out += escapeManifestField(row.path);
        out += '\t';
        out += std::to_string(row.size);
        out += '\t';
        out += std::to_string(row.mtimeUnix);
        out += '\t';
        if (row.kind == ManifestRow::Kind::File) {
            static constexpr char Alphabet[] = "0123456789abcdef";
            for (int shift = 28; shift >= 0; shift -= 4) {
                out.push_back(Alphabet[(row.crc32 >> shift) & 0xf]);
            }
        }
        out += '\t';
        if (row.kind == ManifestRow::Kind::File) {
            out += hashing::toHex(row.sha256);
        }
        out += '\t';
        out += escapeManifestField(row.extra);
        out += '\n';
    }

    hashing::Sha256Digest digest = hashing::Sha256::of(out);
    out += TrailerPrefix;
    out += hashing::toHex(digest);
    out += '\n';
    return out;
}

std::optional<BackupManifest> BackupManifest::parse(std::string_view text, std::string *error)
{
    // Trailer first: locate the last line and check the hash over
    // everything before it, before believing a single field.
    if (text.empty() || text.back() != '\n') {
        fail(error, "manifest does not end with a newline");
        return std::nullopt;
    }
    std::size_t trailerStart = text.rfind('\n', text.size() - 2);
    trailerStart = trailerStart == std::string_view::npos ? 0 : trailerStart + 1;
    std::string_view trailer = text.substr(trailerStart, text.size() - 1 - trailerStart);
    if (trailer.substr(0, TrailerPrefix.size()) != TrailerPrefix) {
        fail(error, "manifest has no #sha256 trailer");
        return std::nullopt;
    }
    std::optional<hashing::Sha256Digest> claimed = hashing::digestFromHex(trailer.substr(TrailerPrefix.size()));
    if (!claimed) {
        fail(error, "manifest trailer hash is malformed");
        return std::nullopt;
    }
    std::string_view body = text.substr(0, trailerStart);
    if (hashing::Sha256::of(body) != *claimed) {
        fail(error, "manifest hash mismatch");
        return std::nullopt;
    }

    BackupManifest manifest;
    std::size_t lineStart = 0;
    bool headerSeen = false;
    std::size_t lineNumber = 0;
    while (lineStart < body.size()) {
        std::size_t lineEnd = body.find('\n', lineStart);
        if (lineEnd == std::string_view::npos) {
            fail(error, "manifest body has an unterminated line");
            return std::nullopt;
        }
        std::string_view line = body.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        ++lineNumber;
        std::vector<std::string_view> fields = splitTabs(line);

        if (!headerSeen) {
            // 6 fields: written before the library fingerprint existed.
            if ((fields.size() != 6 && fields.size() != 7) || fields[0] != Magic) {
                fail(error, "manifest header is not a seabass stick manifest");
                return std::nullopt;
            }
            int version = 0;
            if (!parseNumber(fields[1], version) || version != FormatVersion) {
                fail(error, "unsupported manifest version: " + std::string(fields[1]));
                return std::nullopt;
            }
            auto identifier = unescapeManifestField(fields[2]);
            auto label = unescapeManifestField(fields[3]);
            auto status = backupStatusFromString(fields[4]);
            std::int64_t createdAt = 0;
            if (!identifier || !label || !status || !parseNumber(fields[5], createdAt)) {
                fail(error, "manifest header field malformed");
                return std::nullopt;
            }
            manifest.stickIdentifier = *identifier;
            manifest.stickLabel = *label;
            manifest.status = *status;
            manifest.createdAtUnix = createdAt;
            if (fields.size() == 7) {
                auto fingerprint = unescapeManifestField(fields[6]);
                if (!fingerprint) {
                    fail(error, "manifest header fingerprint malformed");
                    return std::nullopt;
                }
                manifest.libraryFingerprint = *fingerprint;
            }
            headerSeen = true;
            continue;
        }

        if (fields.size() != 7 || fields[0].size() != 1 || (fields[0][0] != 'f' && fields[0][0] != 'd')) {
            fail(error, "manifest row " + std::to_string(lineNumber) + " malformed");
            return std::nullopt;
        }
        ManifestRow row;
        row.kind = fields[0][0] == 'd' ? ManifestRow::Kind::Directory : ManifestRow::Kind::File;
        auto path = unescapeManifestField(fields[1]);
        auto extra = unescapeManifestField(fields[6]);
        if (!path || path->empty() || !extra || !parseNumber(fields[2], row.size) || !parseNumber(fields[3], row.mtimeUnix)) {
            fail(error, "manifest row " + std::to_string(lineNumber) + " malformed");
            return std::nullopt;
        }
        row.path = *path;
        row.extra = *extra;
        if (row.kind == ManifestRow::Kind::File) {
            auto digest = hashing::digestFromHex(fields[5]);
            std::uint32_t crc = 0;
            bool crcOk = fields[4].size() == 8;
            for (char c : fields[4]) {
                int nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (nibble < 0) {
                    crcOk = false;
                    break;
                }
                crc = (crc << 4) | static_cast<std::uint32_t>(nibble);
            }
            if (!digest || !crcOk) {
                fail(error, "manifest row " + std::to_string(lineNumber) + " has a malformed hash");
                return std::nullopt;
            }
            row.sha256 = *digest;
            row.crc32 = crc;
        } else if (!fields[4].empty() || !fields[5].empty() || row.size != 0) {
            fail(error, "manifest directory row " + std::to_string(lineNumber) + " carries file fields");
            return std::nullopt;
        }
        manifest.rows.push_back(std::move(row));
    }
    if (!headerSeen) {
        fail(error, "manifest is empty");
        return std::nullopt;
    }
    return manifest;
}

const ManifestRow *BackupManifest::findRow(std::string_view path) const
{
    for (const ManifestRow &row : rows) {
        if (row.path == path) {
            return &row;
        }
    }
    return nullptr;
}

}  // namespace seabass::infrastructure::stick_backup
