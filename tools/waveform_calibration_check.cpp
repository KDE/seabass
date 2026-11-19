// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Runs domain/engine_overview over the reference tracks the measuring
// scripts wrote (one directory each: pwv6_entries.bin, meta.json,
// predicted_overview_raw.bin, denon_overview_raw.bin, and where the
// player made one, denon_rgb_file.bin) and says, per track:
//
//   - whether the C++ overview is the scripts' prediction byte for byte
//     (it should be: same definition, same numbers);
//   - how far it is from what the player wrote, per band, beside the
//     error the scripts measured for the same track;
//   - the same for the .rgb waveform, the sample count and loudness.
//
// The reference tracks are real tracks of a private library, so they
// stay outside the repository and this is a tool, not a test.
//
// usage: waveform_calibration_check <reference directory>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <zlib.h>

#include "domain/engine_overview.hpp"

namespace fs = std::filesystem;
using seabass::domain::BandTriple;
using seabass::domain::EngineOverview;

namespace
{

std::optional<std::string> readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::uint64_t bigEndian(const std::string &bytes, std::size_t at, std::size_t width)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[at + i]);
    }
    return value;
}

// The player's overview container, uncompressed: two big-endian 64-bit
// counts, a big-endian double (samples per point), the points, the
// final triple.
struct RawOverview
{
    std::uint64_t count = 0;
    double samplesPerPoint = 0.0;
    EngineOverview overview;
};

std::optional<RawOverview> parseRaw(const std::string &bytes)
{
    if (bytes.size() < 24) {
        return std::nullopt;
    }
    RawOverview raw;
    raw.count = bigEndian(bytes, 0, 8);
    const std::uint64_t bits = bigEndian(bytes, 16, 8);
    std::memcpy(&raw.samplesPerPoint, &bits, sizeof bits);
    if (bytes.size() != 24 + 3 * (raw.count + 1)) {
        return std::nullopt;
    }
    auto byteAt = [&](std::size_t i) { return static_cast<std::uint8_t>(bytes[i]); };
    for (std::size_t j = 0; j < raw.count; ++j) {
        const std::size_t at = 24 + 3 * j;
        raw.overview.points.push_back({byteAt(at), byteAt(at + 1), byteAt(at + 2)});
    }
    const std::size_t tail = 24 + 3 * raw.count;
    raw.overview.finalTriple = {byteAt(tail), byteAt(tail + 1), byteAt(tail + 2)};
    return raw;
}

// The compressed form, as the player stores it: a big-endian 32-bit
// uncompressed size, then zlib.
std::optional<std::string> inflated(const std::string &blob)
{
    if (blob.size() < 4) {
        return std::nullopt;
    }
    uLongf size = static_cast<uLongf>(bigEndian(blob, 0, 4));
    std::string out(size, '\0');
    if (uncompress(reinterpret_cast<Bytef *>(out.data()), &size,
                   reinterpret_cast<const Bytef *>(blob.data() + 4), static_cast<uLong>(blob.size() - 4))
        != Z_OK) {
        return std::nullopt;
    }
    out.resize(size);
    return out;
}

std::array<double, 3> meanAbsError(const EngineOverview &a, const EngineOverview &b)
{
    std::array<double, 3> sum{};
    const std::size_t n = std::min(a.points.size(), b.points.size());
    for (std::size_t j = 0; j < n; ++j) {
        sum[0] += std::abs(int(a.points[j].low) - int(b.points[j].low));
        sum[1] += std::abs(int(a.points[j].mid) - int(b.points[j].mid));
        sum[2] += std::abs(int(a.points[j].high) - int(b.points[j].high));
    }
    for (double &s : sum) {
        s /= static_cast<double>(n == 0 ? 1 : n);
    }
    return sum;
}

std::string triple(const BandTriple &t)
{
    return "(" + std::to_string(t.low) + "," + std::to_string(t.mid) + "," + std::to_string(t.high) + ")";
}

std::string bands(const std::array<double, 3> &v, int decimals = 2)
{
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "[%.*f %.*f %.*f]", decimals, v[0], decimals, v[1], decimals, v[2]);
    return buffer;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: waveform_calibration_check <reference directory>\n";
        return 2;
    }
    std::vector<fs::path> dirs;
    for (const auto &entry : fs::directory_iterator(argv[1])) {
        if (entry.is_directory()) {
            dirs.push_back(entry.path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    if (dirs.empty()) {
        std::cerr << "no reference tracks under " << argv[1] << "\n";
        return 1;
    }

    int failures = 0;
    for (const auto &dir : dirs) {
        const auto pwv6 = readFile(dir / "pwv6_entries.bin");
        const auto metaText = readFile(dir / "meta.json");
        const auto predictedBytes = readFile(dir / "predicted_overview_raw.bin");
        const auto denonBytes = readFile(dir / "denon_overview_raw.bin");
        if (!pwv6 || !metaText || !predictedBytes || !denonBytes) {
            std::cout << dir.filename().string() << ": FAIL, a reference file is missing\n";
            ++failures;
            continue;
        }
        const QJsonObject meta = QJsonDocument::fromJson(QByteArray::fromStdString(*metaText)).object();
        const auto scrollEntries = static_cast<std::uint32_t>(meta.value("pwv7_entries").toDouble());
        const auto samples = static_cast<std::uint64_t>(meta.value("samples").toDouble());
        const double sampleRate = meta.value("sample_rate").toDouble();
        const bool isMp3 = meta.value("ext").toString() == QStringLiteral("mp3");

        const EngineOverview ours = seabass::domain::overviewFromThreeBandPreview(
            *pwv6, scrollEntries / 150.0, static_cast<double>(samples) / sampleRate);
        const auto predicted = parseRaw(*predictedBytes);
        const auto denon = parseRaw(*denonBytes);
        if (!predicted || !denon || ours.points.size() != EngineOverview::PointCount) {
            std::cout << dir.filename().string() << ": FAIL, could not read or convert\n";
            ++failures;
            continue;
        }

        std::cout << dir.filename().string() << " (" << (isMp3 ? "mp3" : "not mp3") << ", " << pwv6->size() / 3
                  << " PWV6 entries, " << scrollEntries << " PWV7 entries)\n";

        // Against the scripts' prediction: every byte.
        std::size_t differing = 0;
        int largest = 0;
        for (std::size_t j = 0; j < ours.points.size(); ++j) {
            const auto &a = ours.points[j];
            const auto &b = predicted->overview.points[j];
            const int d[3] = {std::abs(a.low - b.low), std::abs(a.mid - b.mid), std::abs(a.high - b.high)};
            for (int x : d) {
                differing += x != 0 ? 1 : 0;
                largest = std::max(largest, x);
            }
        }
        const bool sameTail = ours.finalTriple == predicted->overview.finalTriple;
        const double ourSpp = static_cast<double>(samples) / 1024.0;
        std::cout << "  vs prediction: " << differing << " of 3072 bytes differ (largest by " << largest
                  << "), final triple " << (sameTail ? "same" : "DIFFERENT") << ", samples/point "
                  << (ourSpp == predicted->samplesPerPoint ? "same" : "DIFFERENT") << "\n";
        if (differing != 0 || !sameTail) {
            ++failures;
        }

        // Against the player.
        const auto error = meanAbsError(ours, denon->overview);
        std::array<double, 3> expected{};
        const QJsonArray expectedArray = meta.value("predicted_mae_per_band").toArray();
        for (int b = 0; b < 3 && b < expectedArray.size(); ++b) {
            expected[static_cast<std::size_t>(b)] = expectedArray.at(b).toDouble();
        }
        std::cout << "  vs player:     MAE L/M/H " << bands(error) << " (scripts measured " << bands(expected, 3)
                  << "), mean " << (error[0] + error[1] + error[2]) / 3.0 << "; final triple ours "
                  << triple(ours.finalTriple) << " player " << triple(denon->overview.finalTriple)
                  << ", player's own is its maximum: "
                  << (seabass::domain::perBandMaximum(denon->overview.points) == denon->overview.finalTriple ? "yes"
                                                                                                             : "NO")
                  << "\n";

        // The .rgb waveform, from our overview.
        if (const auto rgbBlob = readFile(dir / "denon_rgb_file.bin")) {
            const auto rgbRaw = inflated(*rgbBlob);
            const auto rgb = rgbRaw ? parseRaw(*rgbRaw) : std::nullopt;
            if (rgb && rgb->overview.points.size() == EngineOverview::PointCount) {
                const auto second = seabass::domain::secondWaveformFromOverview(ours);
                const auto secondFromPlayers = seabass::domain::secondWaveformFromOverview(denon->overview);
                std::cout << "  .rgb:          MAE from our overview " << bands(meanAbsError(second, rgb->overview))
                          << ", from the player's overview "
                          << bands(meanAbsError(secondFromPlayers, rgb->overview)) << "; final triple ours "
                          << triple(second.finalTriple) << " player " << triple(rgb->overview.finalTriple)
                          << " (player's points' maximum " << triple(seabass::domain::perBandMaximum(rgb->overview.points))
                          << ")\n";
            } else {
                std::cout << "  .rgb:          could not read the player's file\n";
            }
        }

        // Sample count and loudness.
        const std::uint64_t count = seabass::domain::engineSampleCount(scrollEntries, sampleRate, isMp3);
        const double off = (static_cast<double>(count) - static_cast<double>(samples)) / sampleRate * 1000.0;
        std::cout << "  samples:       ours " << count << " player " << samples << " ("
                  << (count == samples ? "exact" : std::to_string(off) + " ms") << ")\n";
        if (isMp3 && count != samples) {
            ++failures;
        }
        const auto loudness = seabass::domain::loudnessFromOverview(ours);
        const QJsonArray playerLoudness = meta.value("trackData").toObject().value("avg_loudness").toArray();
        std::array<double, 3> theirs{};
        for (int b = 0; b < 3 && b < playerLoudness.size(); ++b) {
            theirs[static_cast<std::size_t>(b)] = playerLoudness.at(b).toDouble();
        }
        std::cout << "  loudness:      ours " << bands(loudness, 3) << " player " << bands(theirs, 3) << "\n";
    }
    std::cout << (failures == 0 ? "all reference tracks as expected\n" : std::to_string(failures) + " FAILED\n");
    return failures == 0 ? 0 : 1;
}
