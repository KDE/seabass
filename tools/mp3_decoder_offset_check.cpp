// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Puts the #50 lead-in model to the test against the two decoders it
// describes, on the click files in tests/fixtures/mp3-click.
//
//   mp3_decoder_offset_check FILE...
//
// Each file is decoded whole three times:
//   rekordbox  libmpg123 with MPG123_GAPLESS cleared, twice: once with
//              MPG123_IGNORE_INFOFRAME set, so a Xing/Info frame decodes
//              as a frame of silence, and once without, so libmpg123
//              skips it. Both leads are printed.
//   Engine     libavformat + libavcodec at their defaults, the skip the
//              demuxer asks for (AV_PKT_DATA_SKIP_SAMPLES) applied by the
//              decoder.
//
// Which of the two libmpg123 lanes rekordbox is is NOT measured here. The
// model says rekordbox plays the Info frame as silence when its channel
// mode is the first audio frame's and skips it otherwise; that is an
// assumption from rbxport's static analysis of rekordbox 7 (PR 295), and
// only rekordbox itself can confirm it (the round on neo). This tool
// takes the lane the model selects for the file, so on files with an
// Info frame it checks libmpg123's frame arithmetic and FFmpeg's trim,
// not the Info-frame half of the model.
// In each, the click is the largest |sample| of the first two seconds,
// channels averaged. The measured lag (rekordbox index minus Engine index)
// is compared with engineToRekordboxOffsetMs() at the file's rate; a file
// whose lead-in cannot be read is predicted 0, as the app corrects it.
//
// "lead" is how far each decoder put the click after where it is in the
// encoded PCM (1.5 s): what that decoder adds in front of the audio.
//
// PASS when lag and prediction differ by at most 2 samples. Exit code 1
// when any file fails or cannot be decoded.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
#include <mpg123.h>
}

#include "domain/mp3_lead_in.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"

namespace
{

constexpr long ToleranceSamples = 2;

struct Decoded
{
    std::vector<float> mono;
    int sampleRate = 0;
};

std::optional<Decoded> decodeAsRekordbox(const std::string &path, bool ignoreInfoFrame)
{
    mpg123_handle *mh = mpg123_new(nullptr, nullptr);
    if (!mh) {
        return std::nullopt;
    }
    mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
    mpg123_param(mh, MPG123_REMOVE_FLAGS, MPG123_GAPLESS, 0);
    if (ignoreInfoFrame) {
        mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_IGNORE_INFOFRAME, 0);
    }
    mpg123_format_none(mh);
    const long *rates = nullptr;
    size_t rateCount = 0;
    mpg123_rates(&rates, &rateCount);
    for (size_t i = 0; i < rateCount; ++i) {
        mpg123_format(mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_FLOAT_32);
    }
    if (mpg123_open(mh, path.c_str()) != MPG123_OK) {
        mpg123_delete(mh);
        return std::nullopt;
    }
    Decoded out;
    int channels = 0;
    std::vector<unsigned char> buffer(1 << 16);
    for (;;) {
        size_t done = 0;
        const int err = mpg123_read(mh, buffer.data(), buffer.size(), &done);
        if (err == MPG123_NEW_FORMAT) {
            long rate = 0;
            int encoding = 0;
            mpg123_getformat(mh, &rate, &channels, &encoding);
            out.sampleRate = static_cast<int>(rate);
        }
        if (channels > 0) {
            const float *f = reinterpret_cast<const float *>(buffer.data());
            const size_t frames = done / sizeof(float) / channels;
            for (size_t i = 0; i < frames; ++i) {
                float sum = 0;
                for (int c = 0; c < channels; ++c) {
                    sum += f[i * channels + c];
                }
                out.mono.push_back(sum / channels);
            }
        }
        if (err == MPG123_DONE) {
            break;
        }
        if (err != MPG123_OK && err != MPG123_NEW_FORMAT) {
            std::fprintf(stderr, "  libmpg123: %s\n", mpg123_strerror(mh));
            mpg123_close(mh);
            mpg123_delete(mh);
            return std::nullopt;
        }
    }
    mpg123_close(mh);
    mpg123_delete(mh);
    return out;
}

void appendMono(const AVFrame *frame, std::vector<float> &mono)
{
    const int channels = frame->ch_layout.nb_channels;
    const auto format = static_cast<AVSampleFormat>(frame->format);
    const bool planar = av_sample_fmt_is_planar(format);
    auto sampleAt = [&](int c, int i) -> float {
        const int plane = planar ? c : 0;
        const int index = planar ? i : i * channels + c;
        switch (av_get_packed_sample_fmt(format)) {
        case AV_SAMPLE_FMT_FLT:
            return reinterpret_cast<const float *>(frame->extended_data[plane])[index];
        case AV_SAMPLE_FMT_S16:
            return reinterpret_cast<const int16_t *>(frame->extended_data[plane])[index] / 32768.0f;
        case AV_SAMPLE_FMT_S32:
            return reinterpret_cast<const int32_t *>(frame->extended_data[plane])[index] / 2147483648.0f;
        default:
            std::fprintf(stderr, "  libavcodec: unhandled sample format %s\n", av_get_sample_fmt_name(format));
            std::exit(1);
        }
    };
    for (int i = 0; i < frame->nb_samples; ++i) {
        float sum = 0;
        for (int c = 0; c < channels; ++c) {
            sum += sampleAt(c, i);
        }
        mono.push_back(sum / channels);
    }
}

std::optional<Decoded> decodeAsEngine(const std::string &path)
{
    AVFormatContext *format = nullptr;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) {
        return std::nullopt;
    }
    std::optional<Decoded> result;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    const AVCodec *decoder = nullptr;
    int stream = -1;
    if (avformat_find_stream_info(format, nullptr) >= 0) {
        stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    }
    if (stream >= 0 && decoder) {
        codec = avcodec_alloc_context3(decoder);
    }
    if (codec && avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) >= 0
        && avcodec_open2(codec, decoder, nullptr) >= 0) {
        Decoded out;
        auto drain = [&]() {
            while (avcodec_receive_frame(codec, frame) >= 0) {
                out.sampleRate = frame->sample_rate;
                appendMono(frame, out.mono);
                av_frame_unref(frame);
            }
        };
        while (av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == stream) {
                avcodec_send_packet(codec, packet);
                drain();
            }
            av_packet_unref(packet);
        }
        avcodec_send_packet(codec, nullptr);
        drain();
        result = std::move(out);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    return result;
}

// Index of the largest |sample| in the first two seconds, or -1.
long clickIndex(const Decoded &decoded)
{
    const size_t end = std::min(decoded.mono.size(), static_cast<size_t>(decoded.sampleRate) * 2);
    long best = -1;
    float bestValue = 0;
    for (size_t i = 0; i < end; ++i) {
        if (std::fabs(decoded.mono[i]) > bestValue) {
            bestValue = std::fabs(decoded.mono[i]);
            best = static_cast<long>(i);
        }
    }
    return best;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s FILE...\n", argv[0]);
        return 2;
    }
    av_log_set_level(AV_LOG_ERROR);
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        std::printf("%s\n", path.c_str());
        const auto leadIn = seabass::infrastructure::audio::readMp3LeadIn(path);
        // The lane the model says rekordbox is: the tag frame played as
        // silence when its mode is the audio's. Assumed, not measured.
        const bool ignoreInfoFrame = leadIn && leadIn->infoFrame && leadIn->infoFrameModeMatchesAudio;
        const auto played = decodeAsRekordbox(path, true);
        const auto skipped = decodeAsRekordbox(path, false);
        const auto &rekordbox = ignoreInfoFrame ? played : skipped;
        const auto engine = decodeAsEngine(path);
        if (!played || !skipped || !engine || rekordbox->sampleRate <= 0
            || rekordbox->sampleRate != engine->sampleRate) {
            std::printf("  FAIL: could not decode (libmpg123 %s, libavformat %s)\n",
                        played && skipped ? "ok" : "failed", engine ? "ok" : "failed");
            ++failures;
            continue;
        }
        const int rate = rekordbox->sampleRate;
        const long clickAt = rate * 3L / 2;
        const long playedIndex = clickIndex(*played);
        const long skippedIndex = clickIndex(*skipped);
        const long rbIndex = ignoreInfoFrame ? playedIndex : skippedIndex;
        const long engineIndex = clickIndex(*engine);
        const long lag = rbIndex - engineIndex;
        if (rbIndex < 0 || engineIndex < 0) {
            std::printf("  FAIL: no click in the first two seconds (libmpg123 %ld, libavformat %ld)\n", rbIndex,
                        engineIndex);
            ++failures;
            continue;
        }
        const double predictedMs = leadIn ? seabass::domain::engineToRekordboxOffsetMs(*leadIn) : 0.0;
        const long predicted = std::lround(predictedMs * rate / 1000.0);
        const bool pass = std::labs(lag - predicted) <= ToleranceSamples;
        std::printf("  rate %d\n", rate);
        std::printf("  libmpg123, Info frame as silence  click at %ld  (lead %+ld)%s\n", playedIndex,
                    playedIndex - clickAt, ignoreInfoFrame ? "  <- rekordbox per the model" : "");
        std::printf("  libmpg123, Info frame skipped     click at %ld  (lead %+ld)%s\n", skippedIndex,
                    skippedIndex - clickAt, ignoreInfoFrame ? "" : "  <- rekordbox per the model");
        if (leadIn && leadIn->infoFrame) {
            std::printf("  (which lane rekordbox uses is the model's assumption from rbxport's analysis of\n"
                        "   rekordbox 7; only rekordbox itself measures it)\n");
        }
        std::printf("  Engine (libavformat)              click at %ld  (lead %+ld)\n", engineIndex,
                    engineIndex - clickAt);
        std::printf("  measured lag %ld samples = %.3f ms; model %.3f ms = %ld samples: %s\n", lag,
                    lag * 1000.0 / rate, predictedMs, predicted, pass ? "PASS" : "FAIL");
        if (!pass) {
            ++failures;
        }
    }
    std::printf("%d of %d files agree with the model\n", argc - 1 - failures, argc - 1);
    return failures == 0 ? 0 : 1;
}
