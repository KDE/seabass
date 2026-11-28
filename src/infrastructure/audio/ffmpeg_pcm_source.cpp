// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/ffmpeg_pcm_source.hpp"

#include <algorithm>
#include <vector>

#include "infrastructure/audio/ffmpeg_audio_reader.hpp"

namespace seabass::infrastructure::audio
{

struct FfmpegPcmSource::State
{
    explicit State(const std::string &file)
        : reader(file)
    {
    }

    FfmpegAudioReader reader;
    int sampleRate = 0;
    std::string error;
    // Decoded and not yet read: samples[position..].
    std::vector<float> samples;
    std::size_t position = 0;
    bool finished = false;

    // The next frame as the mono mix the analyser hears, as
    // QtMultimediaPcmSource's: the mean of the channels.
    void decodeMore()
    {
        samples.clear();
        position = 0;
        while (samples.empty() && !finished && error.empty()) {
            const std::optional<FfmpegAudioReader::Chunk> chunk = reader.next();
            if (!chunk) {
                error = reader.error();
                finished = true;
                return;
            }
            if (sampleRate == 0) {
                sampleRate = chunk->sampleRate;
            } else if (chunk->sampleRate != sampleRate) {
                error = "the decoder changed the sample rate part of the way through";
                return;
            }
            samples.reserve(chunk->frames);
            const std::size_t channels = static_cast<std::size_t>(chunk->channels);
            for (std::size_t frame = 0; frame < chunk->frames; ++frame) {
                double sum = 0.0;
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    sum += chunk->samples[frame * channels + channel];
                }
                samples.push_back(static_cast<float>(sum / static_cast<double>(channels)));
            }
        }
    }
};

FfmpegPcmSource::FfmpegPcmSource(const std::string &absoluteFilePath)
    : d(std::make_unique<State>(absoluteFilePath))
{
    d->error = d->reader.error();
    if (!d->error.empty()) {
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
            if (d->finished || !d->error.empty()) {
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
    return d->reader.damagedPackets();
}

}  // namespace seabass::infrastructure::audio
