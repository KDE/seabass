// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "track_import_cover.hpp"

#include <QBuffer>
#include <QImage>

#include <algorithm>

#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/embedded_artwork.hpp"
#endif

namespace seabass::gui
{

std::optional<CoverPictures> scaleCoverPictures(const std::string &pictureBytes)
{
    if (pictureBytes.empty()) {
        return std::nullopt;
    }
    QImage picture =
        QImage::fromData(reinterpret_cast<const uchar *>(pictureBytes.data()), static_cast<int>(pictureBytes.size()));
    if (picture.isNull() || picture.width() < 1 || picture.height() < 1) {
        return std::nullopt;
    }
    const int side = std::min(picture.width(), picture.height());
    picture = picture.copy((picture.width() - side) / 2, (picture.height() - side) / 2, side, side)
                  .convertToFormat(QImage::Format_RGB888);
    const auto jpeg = [&](int pixels) {
        const QImage scaled = picture.scaled(pixels, pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QByteArray out;
        QBuffer buffer(&out);
        buffer.open(QIODevice::WriteOnly);
        return scaled.save(&buffer, "JPEG", 90) ? out.toStdString() : std::string();
    };
    CoverPictures cover{jpeg(80), jpeg(240), jpeg(std::min(side, 500))};
    if (cover.small.empty() || cover.medium.empty() || cover.large.empty()) {
        return std::nullopt;
    }
    return cover;
}

std::optional<CoverPictures> readCoverPictures(const std::string &audioFile)
{
#ifdef SEABASS_HAVE_TAGLIB
    return scaleCoverPictures(infrastructure::audio::readEmbeddedArtwork(audioFile));
#else
    (void)audioFile;
    return std::nullopt;
#endif
}

}  // namespace seabass::gui
