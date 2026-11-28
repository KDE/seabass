// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/ffmpeg_pcm_source.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/samplefmt.h>
}

namespace seabass::infrastructure::audio
{

namespace
{

std::string ffmpegError(const char *what, int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return std::string(what) + ": " + text;
}

// One sample of a frame as a -1..1 value. `plane` is the channel's own
// for a planar format and the one interleaved plane otherwise, `index`
// the sample's place in it.
bool sampleValue(AVSampleFormat format, const std::uint8_t *plane, std::size_t index, double &value)
{
    switch (format) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P:
        value = (plane[index] - 128.0) / 128.0;
        return true;
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
        value = reinterpret_cast<const std::int16_t *>(plane)[index] / 32768.0;
        return true;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
        value = reinterpret_cast<const std::int32_t *>(plane)[index] / 2147483648.0;
        return true;
    case AV_SAMPLE_FMT_S64:
    case AV_SAMPLE_FMT_S64P:
        value = static_cast<double>(reinterpret_cast<const std::int64_t *>(plane)[index]) / 9223372036854775808.0;
        return true;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
        value = reinterpret_cast<const float *>(plane)[index];
        return true;
    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP:
        value = reinterpret_cast<const double *>(plane)[index];
        return true;
    default:
        return false;
    }
}

}  // namespace

struct FfmpegPcmSource::State
{
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    int stream = -1;
    int sampleRate = 0;
    int damaged = 0;
    std::string error;
    // Decoded and not yet read: samples[position..].
    std::vector<float> samples;
    std::size_t position = 0;
    // No more packets: the decoder is being emptied.
    bool draining = false;
    bool finished = false;

    ~State()
    {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }

    // The mono mix the analyser hears, as QtMultimediaPcmSource's: the
    // mean of the channels.
    bool take(const AVFrame *decoded)
    {
        const int channels = decoded->ch_layout.nb_channels;
        if (channels <= 0 || decoded->nb_samples <= 0) {
            return true;
        }
        if (sampleRate == 0) {
            sampleRate = decoded->sample_rate;
        } else if (decoded->sample_rate != sampleRate) {
            error = "the decoder changed the sample rate part of the way through";
            return false;
        }
        const AVSampleFormat sampleFormat = static_cast<AVSampleFormat>(decoded->format);
        const bool planar = av_sample_fmt_is_planar(sampleFormat) != 0;
        const std::size_t count = static_cast<std::size_t>(decoded->nb_samples);
        samples.reserve(samples.size() + count);
        for (std::size_t i = 0; i < count; ++i) {
            double sum = 0.0;
            for (int channel = 0; channel < channels; ++channel) {
                double value = 0.0;
                const std::uint8_t *plane = planar ? decoded->extended_data[channel] : decoded->extended_data[0];
                const std::size_t index = planar ? i : i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel);
                if (!sampleValue(sampleFormat, plane, index, value)) {
                    error = "the decoder gave samples in a format this cannot read";
                    return false;
                }
                sum += value;
            }
            samples.push_back(static_cast<float>(sum / channels));
        }
        return true;
    }

    // Decodes until there are samples to read, the file has ended, or an
    // error is set.
    void decodeMore()
    {
        while (samples.size() == position && !finished && error.empty()) {
            samples.clear();
            position = 0;
            const int received = avcodec_receive_frame(codec, frame);
            if (received == 0) {
                const bool taken = take(frame);
                av_frame_unref(frame);
                if (!taken) {
                    return;
                }
                continue;
            }
            if (received == AVERROR_EOF) {
                finished = true;
                return;
            }
            if (received != AVERROR(EAGAIN)) {
                error = ffmpegError("decoding failed", received);
                return;
            }
            // The decoder wants more: the next packet of the audio
            // stream, or the word that there is none.
            if (draining) {
                // Emptied and still asking: nothing more will come.
                finished = true;
                return;
            }
            const int read = av_read_frame(format, packet);
            if (read < 0) {
                if (read != AVERROR_EOF) {
                    error = ffmpegError("reading the file failed", read);
                    return;
                }
                draining = true;
                avcodec_send_packet(codec, nullptr);
                continue;
            }
            if (packet->stream_index == stream) {
                const int sent = avcodec_send_packet(codec, packet);
                if (sent == AVERROR_INVALIDDATA) {
                    // A damaged packet: left out, and the decode goes on.
                    ++damaged;
                } else if (sent < 0 && sent != AVERROR(EAGAIN)) {
                    error = ffmpegError("decoding failed", sent);
                }
            }
            av_packet_unref(packet);
        }
    }
};

FfmpegPcmSource::FfmpegPcmSource(const std::string &absoluteFilePath)
    : d(std::make_unique<State>())
{
    if (absoluteFilePath.empty()) {
        d->error = "no such file: " + absoluteFilePath;
        return;
    }
    // The path as UTF-8, which is what libavformat takes on every
    // platform.
    int result = avformat_open_input(&d->format, absoluteFilePath.c_str(), nullptr, nullptr);
    if (result < 0) {
        d->error = ffmpegError("the file could not be opened", result);
        return;
    }
    result = avformat_find_stream_info(d->format, nullptr);
    if (result < 0) {
        d->error = ffmpegError("the file's streams could not be read", result);
        return;
    }
    const AVCodec *decoder = nullptr;
    d->stream = av_find_best_stream(d->format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (d->stream < 0 || decoder == nullptr) {
        d->error = "the file has no audio this can decode";
        return;
    }
    d->codec = avcodec_alloc_context3(decoder);
    d->packet = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (d->codec == nullptr || d->packet == nullptr || d->frame == nullptr) {
        d->error = "out of memory";
        return;
    }
    result = avcodec_parameters_to_context(d->codec, d->format->streams[d->stream]->codecpar);
    if (result < 0) {
        d->error = ffmpegError("the decoder could not be set up", result);
        return;
    }
    // The stream's time base, so that the decoder can drop an encoder's
    // delay by the packets' own word (an MP3's, an AAC's).
    d->codec->pkt_timebase = d->format->streams[d->stream]->time_base;
    // One thread, on purpose: nothing rewrites the context beside us, and
    // a frame comes out as soon as its packet went in.
    d->codec->thread_count = 1;
    result = avcodec_open2(d->codec, decoder, nullptr);
    if (result < 0) {
        d->error = ffmpegError("the decoder could not be opened", result);
        return;
    }
    d->decodeMore();
    if (d->sampleRate == 0 && d->error.empty()) {
        d->error = "the decoder gave no audio";
    }
}

FfmpegPcmSource::~FfmpegPcmSource() = default;

int FfmpegPcmSource::sampleRate() const
{
    return d->sampleRate;
}

std::size_t FfmpegPcmSource::read(std::span<float> out)
{
    std::size_t written = 0;
    while (written < out.size()) {
        if (d->position >= d->samples.size()) {
            if (d->finished || !d->error.empty() || d->codec == nullptr) {
                break;
            }
            d->decodeMore();
            if (d->position >= d->samples.size()) {
                break;
            }
        }
        const std::size_t count = std::min(out.size() - written, d->samples.size() - d->position);
        std::copy_n(d->samples.begin() + static_cast<std::ptrdiff_t>(d->position), count,
                    out.begin() + static_cast<std::ptrdiff_t>(written));
        d->position += count;
        written += count;
    }
    return written;
}

std::string FfmpegPcmSource::error() const
{
    return d->error;
}

int FfmpegPcmSource::damagedPackets() const
{
    return d->damaged;
}

}  // namespace seabass::infrastructure::audio
