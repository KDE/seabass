// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What recording_encoder_test and recording_tag_writer_test share: WAVs
// written from a known signal, and a decoder of their own that hands back
// every sample of a file, to compare with the signal without going
// through the code under test.

#pragma once

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"

namespace recording_test
{

// The signal of a test WAV: a tone per channel under noise that reaches
// every bit, so a sample that lost its lowest bits does not compare equal.
// Sample `frame`, `channel` at the WAV's width, as a signed value.
// `variant` gives another noise under the same tone: another recording
// of the same format and length.
inline std::int32_t signalSample(std::uint64_t frame, int channel, int bits, int rate, int variant = 0)
{
    // A hash of the position: the same noise each time it is asked for.
    std::uint64_t x = frame * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(channel + 1) * 0xBF58476D1CE4E5B9ULL
        + static_cast<std::uint64_t>(variant) * 0xD6E8FEB86659FD93ULL;
    x ^= x >> 31;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 29;
    const double tone = std::sin(2.0 * 3.14159265358979 * (220.0 + 110.0 * channel) * static_cast<double>(frame) / rate);
    const std::int32_t full = (1 << (bits - 1)) - 1;
    const auto noise = static_cast<std::int32_t>(x % static_cast<std::uint64_t>(full / 4)) - full / 8;
    const auto value = static_cast<std::int32_t>(tone * full * 0.6) + noise;
    return std::max(-full - 1, std::min(full, value));
}

// The sample on the scale of 32 bits, as a decoder of any width hands it.
inline std::int32_t scaled32(std::int32_t sample, int bits)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(sample) << (32 - bits));
}

struct WavSpec
{
    int rate = 44100;
    int channels = 2;
    int bits = 16;
    std::uint64_t frames = 44100;
    bool extensible = false;
    bool rf64 = false;
    // What the data chunk's header says, when it is to lie.
    std::optional<std::uint32_t> declaredDataBytes;
    // What an RF64's ds64 chunk says the data size is, when it is to lie.
    std::optional<std::uint64_t> ds64DataBytes;
    // A chunk before fmt, as recorders write (bext, LIST).
    bool extraChunk = false;
    // Another noise under the tone (signalSample's `variant`).
    int variant = 0;
    // Bytes after the data chunk, as they are written; counted in the
    // RIFF size.
    std::string trailing;
};

inline void put16(std::ofstream &out, std::uint32_t v)
{
    const char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
    out.write(b, 2);
}

inline void put32(std::ofstream &out, std::uint32_t v)
{
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

inline void put64(std::ofstream &out, std::uint64_t v)
{
    put32(out, static_cast<std::uint32_t>(v));
    put32(out, static_cast<std::uint32_t>(v >> 32));
}

// Writes the WAV a piece at a time, never the whole of it in memory.
inline bool writeWav(const std::string &path, const WavSpec &spec)
{
    std::ofstream out(seabass::pathFromUtf8(path), std::ios::binary);
    const std::uint32_t bytesPerSample = static_cast<std::uint32_t>(spec.bits / 8);
    const std::uint32_t blockAlign = bytesPerSample * static_cast<std::uint32_t>(spec.channels);
    const std::uint64_t dataBytes = spec.frames * blockAlign;
    const std::uint32_t fmtSize = spec.extensible ? 40 : 16;
    const std::uint64_t riffSize = 4 + (spec.rf64 ? 8 + 28 : 0) + (spec.extraChunk ? 8 + 6 : 0) + 8 + fmtSize + 8
        + dataBytes + spec.trailing.size();
    out.write(spec.rf64 ? "RF64" : "RIFF", 4);
    put32(out, spec.rf64 ? 0xFFFFFFFFU : static_cast<std::uint32_t>(riffSize));
    out.write("WAVE", 4);
    if (spec.rf64) {
        out.write("ds64", 4);
        put32(out, 28);
        put64(out, riffSize);
        put64(out, spec.ds64DataBytes ? *spec.ds64DataBytes : dataBytes);
        put64(out, spec.frames);
        put32(out, 0);
    }
    if (spec.extraChunk) {
        out.write("LIST", 4);
        put32(out, 5);
        out.write("INFO\0\0", 6);  // five bytes and the pad byte
    }
    out.write("fmt ", 4);
    put32(out, fmtSize);
    put16(out, spec.extensible ? 0xFFFE : 1);
    put16(out, static_cast<std::uint32_t>(spec.channels));
    put32(out, static_cast<std::uint32_t>(spec.rate));
    put32(out, static_cast<std::uint32_t>(spec.rate) * blockAlign);
    put16(out, blockAlign);
    put16(out, static_cast<std::uint32_t>(spec.bits));
    if (spec.extensible) {
        put16(out, 22);
        put16(out, static_cast<std::uint32_t>(spec.bits));
        put32(out, spec.channels == 2 ? 3 : 4);
        const unsigned char pcm[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                       0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        out.write(reinterpret_cast<const char *>(pcm), 16);
    }
    out.write("data", 4);
    put32(out, spec.declaredDataBytes ? *spec.declaredDataBytes
                                      : (spec.rf64 ? 0xFFFFFFFFU : static_cast<std::uint32_t>(dataBytes)));
    std::vector<char> piece;
    constexpr std::uint64_t PieceFrames = 65536;
    for (std::uint64_t first = 0; first < spec.frames; first += PieceFrames) {
        const std::uint64_t count = std::min(PieceFrames, spec.frames - first);
        piece.clear();
        for (std::uint64_t f = first; f < first + count; ++f) {
            for (int c = 0; c < spec.channels; ++c) {
                const auto v = static_cast<std::uint32_t>(signalSample(f, c, spec.bits, spec.rate, spec.variant));
                for (std::uint32_t b = 0; b < bytesPerSample; ++b) {
                    piece.push_back(static_cast<char>((v >> (8 * b)) & 0xFF));
                }
            }
        }
        out.write(piece.data(), static_cast<std::streamsize>(piece.size()));
    }
    out.write(spec.trailing.data(), static_cast<std::streamsize>(spec.trailing.size()));
    return static_cast<bool>(out);
}

struct Decoded
{
    std::string error;
    int rate = 0;
    int channels = 0;
    // Every sample, interleaved, on the scale of 32 bits.
    std::vector<std::int32_t> samples;
};

// Every sample of a file through QAudioDecoder in the file's own format.
// For short files only: it holds all of them.
inline Decoded decodeAll(const std::string &path)
{
    Decoded decoded;
    QAudioDecoder decoder;
    decoder.setSource(QUrl::fromLocalFile(QString::fromStdString(path)));
    QEventLoop loop;
    QTimer stall;
    stall.setSingleShot(true);
    QObject::connect(&stall, &QTimer::timeout, &loop, [&]() {
        decoded.error = "the decoder stalled";
        loop.quit();
    });
    const auto take = [&]() {
        while (decoder.bufferAvailable()) {
            const QAudioBuffer buffer = decoder.read();
            if (!buffer.isValid()) {
                continue;
            }
            decoded.rate = buffer.format().sampleRate();
            decoded.channels = buffer.format().channelCount();
            const qsizetype count = buffer.sampleCount();
            for (qsizetype i = 0; i < count; ++i) {
                if (buffer.format().sampleFormat() == QAudioFormat::Int16) {
                    decoded.samples.push_back(scaled32(buffer.constData<qint16>()[i], 16));
                } else if (buffer.format().sampleFormat() == QAudioFormat::Int32) {
                    decoded.samples.push_back(buffer.constData<qint32>()[i]);
                } else {
                    decoded.error = "decoded to a sample format the test does not compare";
                    return;
                }
            }
        }
        stall.start(15000);
    };
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, take);
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, [&]() {
        take();
        loop.quit();
    });
    QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), &loop,
                     [&](QAudioDecoder::Error) {
                         decoded.error = decoder.errorString().toStdString();
                         loop.quit();
                     });
    decoder.start();
    stall.start(15000);
    loop.exec();
    decoder.stop();
    return decoded;
}

// Whether `decoded` is the signal of `spec`, every sample.
inline std::string compareWithSignal(const Decoded &decoded, const WavSpec &spec)
{
    if (!decoded.error.empty()) {
        return decoded.error;
    }
    if (decoded.rate != spec.rate || decoded.channels != spec.channels) {
        return "decoded to " + std::to_string(decoded.rate) + " Hz, " + std::to_string(decoded.channels) + " channels";
    }
    if (decoded.samples.size() != spec.frames * static_cast<std::uint64_t>(spec.channels)) {
        return "decoded to " + std::to_string(decoded.samples.size()) + " samples, not "
            + std::to_string(spec.frames * static_cast<std::uint64_t>(spec.channels));
    }
    for (std::uint64_t f = 0; f < spec.frames; ++f) {
        for (int c = 0; c < spec.channels; ++c) {
            const std::int32_t want = scaled32(signalSample(f, c, spec.bits, spec.rate, spec.variant), spec.bits);
            if (decoded.samples[f * static_cast<std::uint64_t>(spec.channels) + static_cast<std::uint64_t>(c)] != want) {
                return "sample of frame " + std::to_string(f) + ", channel " + std::to_string(c) + " differs";
            }
        }
    }
    return {};
}

// The MD5 FLAC's STREAMINFO holds for the signal of `spec`, computed from
// the generated samples (each little endian, in bits / 8 bytes,
// interleaved), so that it goes through no decoder at all.
inline std::array<std::uint8_t, 16> signalMd5(const WavSpec &spec)
{
    QCryptographicHash md5(QCryptographicHash::Md5);
    const int bytes = spec.bits / 8;
    std::vector<char> piece;
    constexpr std::uint64_t PieceFrames = 65536;
    for (std::uint64_t first = 0; first < spec.frames; first += PieceFrames) {
        const std::uint64_t count = std::min(PieceFrames, spec.frames - first);
        piece.clear();
        for (std::uint64_t f = first; f < first + count; ++f) {
            for (int c = 0; c < spec.channels; ++c) {
                const auto v = static_cast<std::uint32_t>(signalSample(f, c, spec.bits, spec.rate, spec.variant));
                for (int b = 0; b < bytes; ++b) {
                    piece.push_back(static_cast<char>((v >> (8 * b)) & 0xFF));
                }
            }
        }
        md5.addData(QByteArrayView(piece.data(), static_cast<qsizetype>(piece.size())));
    }
    const QByteArray digest = md5.result();
    std::array<std::uint8_t, 16> out{};
    std::copy(digest.begin(), digest.end(), out.begin());
    return out;
}

}  // namespace recording_test
