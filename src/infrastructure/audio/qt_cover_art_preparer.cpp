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
        QImage picture = reader.read();
        if (picture.isNull()) {
            result.error = "the picture could not be read: " + reader.errorString().toStdString();
            return result;
        }
        if (picture.width() > application::MaxCoverSidePixels || picture.height() > application::MaxCoverSidePixels) {
            picture = picture.scaled(application::MaxCoverSidePixels, application::MaxCoverSidePixels,
                                     Qt::KeepAspectRatio, Qt::SmoothTransformation);
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
