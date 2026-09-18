// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_xml_writer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace seabass::infrastructure::rekordbox
{

namespace
{

// rekordbox stores a star rating as stars * 51 (0, 51, ... 255), not 0-5.
// Writing 0-5 imports as "almost unrated" on every track and looks like
// the ratings were lost.
constexpr int RatingPerStar = 51;

std::string formatDouble(double value, int decimals)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

std::string attribute(const std::string &name, const std::string &value)
{
    return " " + name + "=\"" + escapeXmlText(value) + "\"";
}

std::string attribute(const std::string &name, long long value)
{
    return " " + name + "=\"" + std::to_string(value) + "\"";
}

// What rekordbox puts in Kind. It is cosmetic in the browser, but it is
// also how rekordbox decides whether it recognises the file at all, so it
// tracks the extension rather than anything we sniff.
std::string kindForPath(const std::string &path)
{
    auto dot = path.rfind('.');
    if (dot == std::string::npos) {
        return "MP3 File";
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == "m4a" || ext == "mp4" || ext == "aac") {
        return "M4A File";
    }
    if (ext == "flac") {
        return "FLAC File";
    }
    if (ext == "wav" || ext == "wave") {
        return "WAV File";
    }
    if (ext == "aif" || ext == "aiff" || ext == "aifc") {
        return "AIFF File";
    }
    return "MP3 File";
}

// "#RRGGBB" (what both readers normalize to) -> the three separate
// attributes rekordbox's XML wants. No color -> no attributes at all,
// which is how an uncolored cue round-trips as uncolored rather than as
// black -- a real color a DJ may have chosen.
std::string colorAttributes(const std::string &color)
{
    if (color.size() != 7 || color[0] != '#') {
        return {};
    }
    auto component = [&color](size_t offset) -> int {
        auto hexDigit = [](char c) -> int {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            return -1;
        };
        const int high = hexDigit(color[offset]);
        const int low = hexDigit(color[offset + 1]);
        if (high < 0 || low < 0) {
            return -1;
        }
        return high * 16 + low;
    };
    const int r = component(1);
    const int g = component(3);
    const int b = component(5);
    if (r < 0 || g < 0 || b < 0) {
        return {};
    }
    return attribute("Red", r) + attribute("Green", g) + attribute("Blue", b);
}

std::string positionMark(const domain::CuePoint &cue)
{
    // Num is the hot cue slot: 0-7 for A-H, -1 for a memory cue. Type is
    // 0 for a plain cue and 4 for a loop, which additionally carries End.
    const int num = cue.kind == domain::CuePoint::Kind::Hot ? cue.hotCueNumber : -1;
    // Clamped at zero: Engine rounds its own analysis-placed cues a hair
    // below the first sample (-0.000, -0.052 seen on a real stick), and
    // "Start=-0.000" is not a position rekordbox can seek to. Junk memory
    // cues there are dropped long before this, but a deliberate hot cue at
    // the very start has to survive, and it can only survive as 0.
    const double startSeconds = std::max(0.0, cue.positionMs / 1000.0);
    std::string mark = "      <POSITION_MARK";
    mark += attribute("Name", cue.comment);
    mark += attribute("Type", cue.isLoop ? 4 : 0);
    mark += attribute("Start", formatDouble(startSeconds, 3));
    if (cue.isLoop) {
        mark += attribute("End", formatDouble(std::max(startSeconds, cue.loopEndMs / 1000.0), 3));
    }
    mark += attribute("Num", num);
    mark += colorAttributes(cue.color);
    mark += "/>\n";
    return mark;
}

std::string trackElement(const domain::Track &track, long long trackId)
{
    std::string element = "    <TRACK";
    element += attribute("TrackID", trackId);
    element += attribute("Name", track.title);
    element += attribute("Artist", track.artist);
    element += attribute("Album", track.album);
    element += attribute("Kind", kindForPath(track.filePath.empty() ? track.filename : track.filePath));
    if (track.fileSizeBytes > 0) {
        element += attribute("Size", static_cast<long long>(track.fileSizeBytes));
    }
    // TotalTime is whole seconds; rekordbox reads no more precision here
    // and the real duration comes from its own analysis anyway.
    element += attribute("TotalTime", static_cast<long long>(track.durationSeconds));
    if (track.bpm > 0.0) {
        element += attribute("AverageBpm", formatDouble(track.bpm, 2));
    }
    if (track.bitrate > 0) {
        element += attribute("BitRate", track.bitrate);
    }
    if (!track.key.empty()) {
        element += attribute("Tonality", track.key);
    }
    if (!track.comment.empty()) {
        element += attribute("Comments", track.comment);
    }
    if (track.playCount.has_value()) {
        element += attribute("PlayCount", *track.playCount);
    }
    if (track.rating.has_value()) {
        element += attribute("Rating", *track.rating * RatingPerStar);
    }
    element += attribute("Location", toRekordboxLocation(track.filePath));

    std::vector<std::string> children;
    // One TEMPO, at the first beat. Seabass reads beatgrids but never
    // writes them (anlz_file.hpp copies every non-cue section through
    // verbatim), so this is the honest half of a grid: the tempo, anchored
    // where the catalog says the first beat is. rekordbox re-analysis
    // fills in the rest.
    if (track.bpm > 0.0) {
        children.push_back("      <TEMPO" + attribute("Inizio", formatDouble(0.0, 3)) +
                            attribute("Bpm", formatDouble(track.bpm, 2)) + attribute("Metro", std::string("4/4")) +
                            attribute("Battito", 1) + "/>\n");
    }
    for (const auto &cue : track.cues) {
        children.push_back(positionMark(cue));
    }

    if (children.empty()) {
        return element + "/>\n";
    }
    element += ">\n";
    for (const auto &child : children) {
        element += child;
    }
    element += "    </TRACK>\n";
    return element;
}

void writeNode(const domain::XmlPlaylistNode &node, const std::vector<long long> &trackIds, int depth,
                std::string &out)
{
    const std::string indent(static_cast<size_t>(depth) * 2 + 4, ' ');

    // rekordbox has no node that is both a folder and a playlist. A path
    // that is used as both -- tracks sitting directly in "Techno" while
    // "Techno/Peak Time" also exists -- becomes a folder holding a
    // same-named playlist, which is what rekordbox itself does when you
    // drag tracks onto a folder.
    const bool hasTracks = !node.trackIndices.empty();
    // The root is a folder even when it is empty: rekordbox's importer
    // looks for <NODE Type="0" Name="ROOT"> and shows nothing at all if
    // the outermost node is a playlist instead.
    const bool hasChildren = !node.children.empty() || depth == 0;

    if (hasChildren) {
        out += indent + "<NODE" + attribute("Name", node.name) + attribute("Type", 0) +
               attribute("Count", static_cast<long long>(node.children.size() + (hasTracks ? 1 : 0))) + ">\n";
        for (const auto &child : node.children) {
            writeNode(child, trackIds, depth + 1, out);
        }
        if (hasTracks) {
            domain::XmlPlaylistNode leaf;
            leaf.name = node.name;
            leaf.trackIndices = node.trackIndices;
            writeNode(leaf, trackIds, depth + 1, out);
        }
        out += indent + "</NODE>\n";
        return;
    }

    out += indent + "<NODE" + attribute("Name", node.name) + attribute("Type", 1) + attribute("KeyType", 0) +
           attribute("Entries", static_cast<long long>(node.trackIndices.size())) + ">\n";
    for (auto index : node.trackIndices) {
        if (index >= trackIds.size()) {
            continue;
        }
        out += indent + "  <TRACK" + attribute("Key", trackIds[index]) + "/>\n";
    }
    out += indent + "</NODE>\n";
}

}  // namespace

std::string escapeXmlText(const std::string &text)
{
    // Escaping the five entities is the easy half. The hard half is that
    // tags on a real stick contain bytes XML 1.0 simply cannot represent,
    // and a single one of them makes the whole document unparseable --
    // rekordbox rejects the file, with no clue which of 1471 tracks did
    // it. Found on the WHALESHARK stick: artist "A<U+FFFD><0x0C>", a form
    // feed inside an ID3 frame. So this also drops the control characters
    // XML forbids (everything below 0x20 except tab, LF and CR) and
    // replaces malformed UTF-8 with U+FFFD rather than passing it
    // through.
    std::string out;
    out.reserve(text.size() + 16);

    const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
    const size_t size = text.size();

    auto appendReplacement = [&out]() {
        out += "\xEF\xBF\xBD";  // U+FFFD
    };

    for (size_t i = 0; i < size;) {
        const unsigned char c = bytes[i];

        if (c < 0x80) {
            switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            case '\t':
                // Legal, but an attribute value would have it normalized
                // to a space on parse. A character reference survives.
                out += "&#x9;";
                break;
            case '\n':
                out += "&#xA;";
                break;
            case '\r':
                out += "&#xD;";
                break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    break;  // illegal in XML 1.0: dropped, not escaped
                }
                out.push_back(static_cast<char>(c));
                break;
            }
            ++i;
            continue;
        }

        // Multi-byte UTF-8: validate the whole sequence before trusting
        // any of it, including the over-long and out-of-range forms that
        // would otherwise smuggle an illegal character past the check.
        size_t length = 0;
        unsigned int codepoint = 0;
        if ((c & 0xE0) == 0xC0) {
            length = 2;
            codepoint = c & 0x1FU;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
            codepoint = c & 0x0FU;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
            codepoint = c & 0x07U;
        } else {
            appendReplacement();
            ++i;
            continue;
        }

        if (i + length > size) {
            appendReplacement();
            ++i;
            continue;
        }
        bool valid = true;
        for (size_t k = 1; k < length; ++k) {
            const unsigned char continuation = bytes[i + k];
            if ((continuation & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (continuation & 0x3FU);
        }
        const bool overlong = (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
                               (length == 4 && codepoint < 0x10000);
        const bool surrogate = codepoint >= 0xD800 && codepoint <= 0xDFFF;
        const bool tooLarge = codepoint > 0x10FFFF;
        // U+FFFE and U+FFFF are valid UTF-8 and invalid XML characters.
        const bool notXmlChar = codepoint == 0xFFFE || codepoint == 0xFFFF;
        if (!valid || overlong || surrogate || tooLarge || notXmlChar) {
            appendReplacement();
            ++i;
            continue;
        }

        out.append(text, i, length);
        i += length;
    }
    return out;
}

std::string toRekordboxLocation(const std::string &absolutePath)
{
    if (absolutePath.empty()) {
        return {};
    }

    std::string path = absolutePath;
    // Windows hands us backslashes; a URL only ever has forward ones.
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.front() != '/') {
        // "C:/Music/..." -> "/C:/Music/...", so the URL keeps its three
        // slashes after the authority.
        path.insert(path.begin(), '/');
    }

    std::string encoded;
    encoded.reserve(path.size() + 16);
    for (unsigned char c : path) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                 c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
        if (unreserved) {
            encoded.push_back(static_cast<char>(c));
            continue;
        }
        char buffer[4];
        std::snprintf(buffer, sizeof(buffer), "%%%02X", c);
        encoded += buffer;
    }
    return "file://localhost" + encoded;
}

std::string writeRekordboxXml(const std::vector<domain::Track> &tracks, const domain::XmlPlaylistNode &playlists,
                                const RekordboxXmlOptions &options)
{
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<DJ_PLAYLISTS Version=\"1.0.0\">\n";
    out += "  <PRODUCT" + attribute("Name", options.productName) + attribute("Version", options.productVersion) +
           attribute("Company", options.productCompany) + "/>\n";
    out += "  <COLLECTION" + attribute("Entries", static_cast<long long>(tracks.size())) + ">\n";

    // TrackID is rekordbox's own key space, referenced by every playlist
    // entry. 1-based and assigned here rather than reusing a catalog's
    // source id: those collide across formats (a rekordbox row 19 and an
    // Engine row 19 are different tracks) and the collapsed track carries
    // several of them at once.
    std::vector<long long> trackIds;
    trackIds.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        trackIds.push_back(static_cast<long long>(i) + 1);
        out += trackElement(tracks[i], trackIds.back());
    }
    out += "  </COLLECTION>\n";

    out += "  <PLAYLISTS>\n";
    std::string nodes;
    writeNode(playlists, trackIds, 0, nodes);
    out += nodes;
    out += "  </PLAYLISTS>\n";
    out += "</DJ_PLAYLISTS>\n";
    return out;
}

}  // namespace seabass::infrastructure::rekordbox
