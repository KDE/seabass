// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/analysis_pcm_source.hpp"

#include <atomic>

#ifdef SEABASS_HAVE_FFMPEG_DECODER
#include "infrastructure/audio/ffmpeg_audio_reader.hpp"
#include "infrastructure/audio/ffmpeg_pcm_source.hpp"
#endif
#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"

namespace seabass::infrastructure::audio
{

namespace
{
#ifdef SEABASS_HAVE_FFMPEG_DECODER
std::atomic<bool> g_ownDecoder{true};
#endif
}  // namespace

bool ownDecoderInUse()
{
#ifdef SEABASS_HAVE_FFMPEG_DECODER
    return g_ownDecoder;
#else
    return false;
#endif
}

void setOwnDecoderInUseForTesting(bool inUse)
{
#ifdef SEABASS_HAVE_FFMPEG_DECODER
    g_ownDecoder = inUse;
#else
    (void)inUse;
#endif
}

std::string ownDecoderVersion()
{
#ifdef SEABASS_HAVE_FFMPEG_DECODER
    if (ownDecoderInUse()) {
        return FfmpegAudioReader::libraryVersion();
    }
#endif
    return {};
}

std::unique_ptr<domain::PcmSource> openAnalysisPcmSource(const std::string &absoluteFilePath)
{
#ifdef SEABASS_HAVE_FFMPEG_DECODER
    if (ownDecoderInUse()) {
        return std::make_unique<FfmpegPcmSource>(absoluteFilePath);
    }
#endif
    return std::make_unique<QtMultimediaPcmSource>(absoluteFilePath);
}

bool analysisDecodingAvailable()
{
    return ownDecoderInUse() || QtMultimediaPcmSource::decodingAvailable();
}

const char *analysisDecoderName()
{
    return ownDecoderInUse() ? "FFmpeg" : "Qt Multimedia";
}

}  // namespace seabass::infrastructure::audio
