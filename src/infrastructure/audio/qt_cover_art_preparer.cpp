// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/qt_cover_art_preparer.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QSize>

#include <algorithm>
#include <exception>
#include <string>

namespace seabass::infrastructure::audio
{

namespace
{

// The first JPEG quality tried, and the lowest one taken before a picture
// is refused as too large; a 1200 pixel cover fits well above it.
constexpr int FirstJpegQuality = 90;
constexpr int LowestJpegQuality = 50;
constexpr int JpegQualityStep = 5;
// The largest picture decoded at all: a side of 16384 pixels and 64
// megapixels (256 MB at four bytes a pixel). A phone's photo of 50
// megapixels is in; a picture whose header claims more is refused before
// anything is decoded.
constexpr qint64 MaxSourceSidePixels = 16384;
constexpr qint64 MaxSourcePixels = 64LL * 1000 * 1000;

// `size` scaled down to fit MaxCoverSidePixels on its longer side, its
// shape kept, never up, and never below one pixel on either side.
QSize fitted(const QSize &size)
{
    const qint64 w = size.width();
    const qint64 h = size.height();
    const qint64 longer = std::max(w, h);
    if (longer <= application::MaxCoverSidePixels) {
        return size;
    }
    const auto scale = [longer](qint64 side) {
        return static_cast<int>(std::max<qint64>(1, (side * application::MaxCoverSidePixels + longer / 2) / longer));
    };
    return {scale(w), scale(h)};
}

}  // namespace

application::CoverArtResult QtCoverArtPreparer::prepare(const std::string &pictureBytes)
{
    application::CoverArtResult result;
    try {
        if (!QImageWriter::supportedImageFormats().contains("jpeg")) {
            result.error = "this system's Qt cannot write JPEG pictures";
            return result;
        }
        QByteArray bytes(pictureBytes.data(), static_cast<qsizetype>(pictureBytes.size()));
        QBuffer in(&bytes);
        in.open(QIODevice::ReadOnly);
        QImageReader reader(&in);
        reader.setAutoTransform(true);
        // The size from the picture's header, before anything is decoded.
        const QSize stored = reader.size();
        if (stored.isValid()) {
            if (stored.width() > MaxSourceSidePixels || stored.height() > MaxSourceSidePixels
                || static_cast<qint64>(stored.width()) * stored.height() > MaxSourcePixels) {
                result.error = "the picture is " + std::to_string(stored.width()) + " x "
                    + std::to_string(stored.height()) + " pixels, larger than a cover is made from";
                return result;
            }
            // Decoded at the cover's size where the format can (JPEG),
            // so a large photo does not cost its full size in memory.
            const QSize cover = fitted(stored);
            if (cover != stored) {
                reader.setScaledSize(cover);
            }
        }
        QImage picture = reader.read();
        if (picture.isNull()) {
            result.error = "the picture could not be read: " + reader.errorString().toStdString();
            return result;
        }
        // A format that did not tell its size, or a turn from the
        // picture's orientation: fitted here.
        const QSize cover = fitted(picture.size());
        if (cover != picture.size()) {
            picture = picture.scaled(cover, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        // JPEG has no transparency: what is see-through is shown on white.
        QImage flat(picture.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        {
            QPainter painter(&flat);
            painter.drawImage(0, 0, picture);
        }

        for (int quality = FirstJpegQuality; quality >= LowestJpegQuality; quality -= JpegQualityStep) {
            QByteArray jpeg;
            QBuffer out(&jpeg);
            out.open(QIODevice::WriteOnly);
            QImageWriter writer(&out, "jpeg");
            writer.setQuality(quality);
            // Baseline: a progressive JPEG is what players leave blank.
            writer.setProgressiveScanWrite(false);
            writer.setOptimizedWrite(false);
            if (!writer.write(flat)) {
                result.error = "the cover could not be written as JPEG: " + writer.errorString().toStdString();
                return result;
            }
            if (static_cast<std::size_t>(jpeg.size()) <= application::MaxCoverBytes) {
                result.cover = application::CoverArt{std::string(jpeg.constData(), static_cast<std::size_t>(jpeg.size())),
                                                     flat.width(), flat.height()};
                return result;
            }
        }
        result.error = "the cover stays larger than " + std::to_string(application::MaxCoverBytes / 1024)
            + " KB even at the lowest quality";
    } catch (const std::exception &e) {
        result = {};
        result.error = std::string("the cover could not be made: ") + e.what();
    }
    return result;
}

}  // namespace seabass::infrastructure::audio
