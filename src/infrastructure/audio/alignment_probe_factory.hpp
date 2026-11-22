// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>

#include "infrastructure/audio/identical_file_alignment_probe.hpp"

#ifdef SEABASS_HAVE_QT_AUDIO
#include "infrastructure/audio/qt_multimedia_alignment_probe.hpp"
#endif

namespace seabass::infrastructure::audio
{

// The one place that decides which AudioAlignmentProbe a caller gets, so
// Cues on Duplicate Copies, Clean Up and the CLI line copies up by one
// rule (silence_probe_factory.hpp exists for the same reason).
//
// Never null: byte-identical copies are lined up by comparing bytes,
// which every build can do. The decoder behind it is there only when this
// build has Qt Multimedia AND its backend loaded; without one, every pair
// that is not byte-identical is unknown, and the caller can ask
// hasDecoder() to say why.
inline std::unique_ptr<IdenticalFileAlignmentProbe> makeAudioAlignmentProbe()
{
    std::unique_ptr<domain::AudioAlignmentProbe> decoder;
#ifdef SEABASS_HAVE_QT_AUDIO
    if (QtMultimediaAlignmentProbe::decodingAvailable()) {
        decoder = std::make_unique<QtMultimediaAlignmentProbe>();
    }
#endif
    return std::make_unique<IdenticalFileAlignmentProbe>(std::move(decoder));
}

}  // namespace seabass::infrastructure::audio
