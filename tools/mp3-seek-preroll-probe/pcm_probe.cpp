// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Decode-to-PCM for #92: decodes FILE once with QAudioDecoder (the same
// FFmpeg the player uses) and writes it as a 16-bit stereo WAV at the
// file's own sample rate, sample n of the WAV being the player's track
// time n / rate. A WAV seeks to the sample, so a hot cue jump in it has
// no bit reservoir to lose.
//
// Each decoded buffer is placed by its start time: the first one starts
// at the MP3's start offset (26 ms or so for a LAME file, issue #50), and
// the WAV gets that much silence in front so the two timelines agree.
// The WAV is written as the buffers arrive (header patched at the end),
// so only one buffer is held at a time; with "memory" as the third
// argument everything is collected first and written at the end, to
// compare what that costs.
//
// Prints the decode time, the process's peak resident memory before and
// after (VmHWM), the format, and any gap or overlap between buffers.
// Linux only (reads /proc/self/status). No sound device is used.
// Built by hand, see README.md.
//
//   pcm_probe FILE OUT.wav [memory]

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

std::string statusLine(const char *key)
{
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind(key, 0) == 0) {
            const auto first = line.find_first_not_of(" \t", std::strlen(key));
            return first == std::string::npos ? line : line.substr(first);
        }
    }
    return "?";
}

void putLe(QByteArray &out, quint32 value, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        out.append(static_cast<char>((value >> (8 * i)) & 0xff));
    }
}

QByteArray wavHeader(int rate, quint32 dataBytes)
{
    QByteArray header;
    header.append("RIFF");
    putLe(header, 36 + dataBytes, 4);
    header.append("WAVEfmt ");
    putLe(header, 16, 4);
    putLe(header, 1, 2);             // PCM
    putLe(header, 2, 2);             // stereo
    putLe(header, rate, 4);
    putLe(header, rate * 4, 4);      // bytes per second
    putLe(header, 4, 2);             // bytes per frame
    putLe(header, 16, 2);            // bits per sample
    header.append("data");
    putLe(header, dataBytes, 4);
    return header;
}

float sampleAt(const QAudioBuffer &buffer, qsizetype frame, int channel)
{
    const QAudioFormat format = buffer.format();
    const qsizetype at = frame * format.channelCount() + std::min(channel, format.channelCount() - 1);
    switch (format.sampleFormat()) {
    case QAudioFormat::Float: return buffer.constData<float>()[at];
    case QAudioFormat::Int16: return buffer.constData<qint16>()[at] / 32768.0F;
    case QAudioFormat::Int32: return static_cast<float>(buffer.constData<qint32>()[at] / 2147483648.0);
    case QAudioFormat::UInt8: return (buffer.constData<quint8>()[at] - 128) / 128.0F;
    default: return 0;
    }
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: pcm_probe FILE OUT.wav [memory]\n");
        return 2;
    }
    const bool inMemory = argc > 3 && std::strcmp(argv[3], "memory") == 0;
    QFile out(QString::fromUtf8(argv[2]));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    std::printf("before: VmHWM %s, VmRSS %s\n", statusLine("VmHWM:").c_str(), statusLine("VmRSS:").c_str());

    QAudioDecoder decoder;
    decoder.setSource(QUrl::fromLocalFile(QString::fromUtf8(argv[1])));
    int rate = 0;
    qint64 written = 0;  // frames
    qint64 gapFrames = 0;
    qint64 overlapFrames = 0;
    int gaps = 0;
    QByteArray held;
    QByteArray chunk;
    bool failed = false;
    double firstStartMs = -1;
    QAudioFormat firstFormat;
    QElapsedTimer clock;

    const auto emitBytes = [&](const QByteArray &bytes) {
        if (inMemory) {
            held.append(bytes);
        } else {
            out.write(bytes);
        }
    };

    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &app, [&] {
        const QAudioBuffer buffer = decoder.read();
        if (!buffer.isValid()) {
            return;
        }
        if (rate == 0) {
            rate = buffer.format().sampleRate();
            firstFormat = buffer.format();
            firstStartMs = buffer.startTime() / 1000.0;
            out.write(wavHeader(rate, 0));  // patched at the end
        }
        if (buffer.format().sampleRate() != rate) {
            std::fprintf(stderr, "the sample rate changed mid-file: %d to %d\n", rate,
                         buffer.format().sampleRate());
            failed = true;
            app.quit();
            return;
        }
        const qint64 at = std::llround(buffer.startTime() * static_cast<double>(rate) / 1e6);
        qsizetype skip = 0;
        if (at > written) {
            if (written > 0) {
                ++gaps;
                gapFrames += at - written;
            }
            emitBytes(QByteArray((at - written) * 4, '\0'));
            written = at;
        } else if (at < written) {
            overlapFrames += written - at;
            skip = written - at;
        }
        const qsizetype frames = buffer.frameCount();
        if (skip >= frames) {
            return;
        }
        chunk.resize((frames - skip) * 4);
        auto *dst = reinterpret_cast<qint16 *>(chunk.data());
        for (qsizetype i = skip; i < frames; ++i) {
            for (int c = 0; c < 2; ++c) {
                const float v = std::clamp(sampleAt(buffer, i, c), -1.0F, 1.0F);
                *dst++ = static_cast<qint16>(std::lrint(v * 32767.0F));
            }
        }
        emitBytes(chunk);
        written += frames - skip;
    });
    QObject::connect(&decoder, &QAudioDecoder::finished, &app, [&] { app.quit(); });
    QObject::connect(&decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), &app, [&] {
        std::fprintf(stderr, "decode failed: %s\n", qPrintable(decoder.errorString()));
        failed = true;
        app.quit();
    });
    clock.start();
    decoder.start();
    app.exec();
    const qint64 decodeMs = clock.elapsed();
    if (failed || rate == 0) {
        return 1;
    }
    if (inMemory) {
        out.write(held);
    }
    const quint64 dataBytes = static_cast<quint64>(written) * 4;
    if (dataBytes > 0xFFFFFFFFULL - 36) {
        std::fprintf(stderr, "over 4 GB of PCM: a RIFF WAV cannot hold it\n");
        return 1;
    }
    out.seek(0);
    out.write(wavHeader(rate, static_cast<quint32>(dataBytes)));
    out.close();
    std::printf("decoded: %.3f s, %d Hz, first buffer at %.3f ms (%d ch, sample format %d), "
                "%d gaps (%lld frames) and %lld overlapping frames after it\n",
                written / double(rate), rate, firstStartMs, firstFormat.channelCount(),
                int(firstFormat.sampleFormat()), gaps, static_cast<long long>(gapFrames),
                static_cast<long long>(overlapFrames));
    std::printf("decode and write: %lld ms, %s; WAV %.1f MB\n", static_cast<long long>(decodeMs),
                inMemory ? "held in memory" : "streamed to the file", dataBytes / 1e6);
    std::printf("after: VmHWM %s, VmRSS %s\n", statusLine("VmHWM:").c_str(), statusLine("VmRSS:").c_str());
    return 0;
}
