// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/analysis_pcm_source.hpp"

#ifdef SEABASS_HAVE_FFMPEG_DECODER
#include "infrastructure/audio/ffmpeg_pcm_source.hpp"
#else
#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"
#endif

namespace seabass::infrastructure::audio
{

std::unique_ptr<domain::PcmSource> openAnalysisPcmSource(const std::string &absoluteFilePath)
{
#ifdef SEABASS_HAVE_FFMPEG_DECODER
    return std::make_unique<FfmpegPcmSource>(absoluteFilePath);
#else
    return std::make_unique<QtMultimediaPcmSource>(absoluteFilePath);
#endif
}

bool analysisDecodingAvailable()
{
#ifdef SEABASS_HAVE_FFMPEG_DECODER
    return true;
#else
    return QtMultimediaPcmSource::decodingAvailable();
#endif
}

const char *analysisDecoderName()
{
#ifdef SEABASS_HAVE_FFMPEG_DECODER
    return "FFmpeg";
#else
    return "Qt Multimedia";
#endif
}

}  // namespace seabass::infrastructure::audio
