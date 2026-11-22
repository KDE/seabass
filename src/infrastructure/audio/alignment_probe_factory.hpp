// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <string>

#include "infrastructure/audio/identical_file_alignment_probe.hpp"
#include "infrastructure/local/alignment_cache.hpp"

#ifdef SEABASS_HAVE_QT_AUDIO
#include <QByteArray>
#include <QtGlobal>

#include "infrastructure/audio/qt_multimedia_alignment_probe.hpp"
#endif

namespace seabass::infrastructure::audio
{

// Whether audio can be decoded here, and if not, why: a build without Qt
// Multimedia, or one whose decoding backend did not load on this
// computer. Two different things to tell a person: the second is not
// "this build cannot".
enum class AudioDecoding { NotInThisBuild, DidNotLoad, Available };

inline AudioDecoding audioDecoding()
{
#ifdef SEABASS_HAVE_QT_AUDIO
    return QtMultimediaAlignmentProbe::decodingAvailable() ? AudioDecoding::Available : AudioDecoding::DidNotLoad;
#else
    return AudioDecoding::NotInThisBuild;
#endif
}

// The decoder a measured shift is on the timeline of, for the stick's
// cache (local/alignment_cache.hpp), as far as it can be named: the Qt
// Multimedia backend asked for (QT_MEDIA_BACKEND), else "default", and the
// Qt that is running. Which backend Qt then picks, and the FFmpeg a
// distribution's Qt links, are not visible here: two machines with the
// same Qt that differ in those share lines. "none" in a build without a
// decoder.
inline std::string alignmentDecoderName()
{
#ifdef SEABASS_HAVE_QT_AUDIO
    const QByteArray backend = qgetenv("QT_MEDIA_BACKEND");
    return (backend.isEmpty() ? std::string("default") : backend.toStdString()) + " qt" + qVersion();
#else
    return "none";
#endif
}

// The one place that decides which AudioAlignmentProbe a caller gets, so
// Cues on Duplicate Copies, Clean Up and the CLI line copies up by one
// rule (silence_probe_factory.hpp exists for the same reason).
//
// Never null: byte-identical copies are lined up by comparing bytes,
// which every build can do. The decoder behind it is there only when this
// build has Qt Multimedia AND its backend loaded; without one, every pair
// that is not byte-identical and not in the stick's cache is unknown, and
// the caller can ask hasDecoder() to say why.
//
// In front of both, the stick's own record of pairs already measured
// (local/alignment_cache.hpp), so a second scan decodes nothing it has
// decoded before. `stickRoot` empty is no cache: every pair is measured.
// `writes` ReadOnly reads the stick's cache and writes nothing back.
inline std::unique_ptr<local::CachedAudioAlignmentProbe> makeAudioAlignmentProbe(
    const std::string &stickRoot,
    local::CachedAudioAlignmentProbe::Writes writes = local::CachedAudioAlignmentProbe::Writes::Yes)
{
    std::unique_ptr<domain::AudioAlignmentProbe> decoder;
#ifdef SEABASS_HAVE_QT_AUDIO
    if (QtMultimediaAlignmentProbe::decodingAvailable()) {
        decoder = std::make_unique<QtMultimediaAlignmentProbe>();
    }
#endif
    const bool decodes = decoder != nullptr;
    return std::make_unique<local::CachedAudioAlignmentProbe>(
        stickRoot, std::make_unique<IdenticalFileAlignmentProbe>(std::move(decoder)), decodes, alignmentDecoderName(),
        writes);
}

}  // namespace seabass::infrastructure::audio
