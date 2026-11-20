// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>

#include "../scratch_path.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/paths/utf8_path.hpp"

// A scratch Engine library of one analysed track for the beat grid tests
// (#40): a minute at 120 BPM, its markers 22050 samples a beat apart (44.1
// kHz), and the row's sample rate and tempo set as a test needs them, so
// that what PlaybackController draws for one row can be watched change.
// Libraries are removed with the fixture.
class EngineGridFixture : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~EngineGridFixture() override
    {
        std::error_code ec;
        for (const QString &root : m_roots) {
            std::filesystem::remove_all(seabass::pathFromUtf8(root.toStdString()), ec);
        }
    }

    // The library's path, or empty on failure (the test then fails on it).
    Q_INVOKABLE QString library()
    {
        namespace fs = std::filesystem;
        static std::atomic<int> made{0};
        const fs::path root = seabass::testing::scratchRoot()
            / ("seabass_qml_engine_grid_" + std::to_string(QCoreApplication::applicationPid()) + "_"
               + std::to_string(made++));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "Contents", ec);
        const fs::path libraryPath = seabass::infrastructure::engine::engineLibraryPath(root);
        seabass::domain::Track track;
        track.sourceId = "1";
        track.title = "Grid";
        track.artist = "An Artist";
        track.filePath = seabass::pathToUtf8(root / "Contents" / "grid.mp3");
        track.bpm = 120.0;
        track.durationSeconds = 60.0;
        const auto created = seabass::infrastructure::engine::EngineLibraryCreator::create(
            seabass::pathToUtf8(libraryPath), {track}, seabass::infrastructure::engine::EngineSchemaGeneration::V2);
        if (!created.errorMessage.empty() || created.tracksCreated != 1) {
            return {};
        }
        m_roots << QString::fromStdString(seabass::pathToUtf8(root));
        const QString path = QString::fromStdString(seabass::pathToUtf8(libraryPath));
        return setRow(path, 44100.0, 120.0) ? path : QString();
    }

    // The library's one track id.
    Q_INVOKABLE QString trackId(const QString &library) const
    {
        try {
            auto db = djinterop::engine::load_database(library.toStdString());
            const auto tracks = db.tracks();
            return tracks.size() == 1 ? QString::number(tracks[0].id()) : QString();
        } catch (const std::exception &) {
            return {};
        }
    }

    // Sets the row's recorded sample rate (0 records none) and its tempo,
    // and puts its markers back 22050 samples a beat apart. With no rate
    // and a tempo the markers do not fit (not 120), the beat reader has
    // no rate it can know, and the track has no grid.
    Q_INVOKABLE bool setRow(const QString &library, double sampleRate, double bpm)
    {
        try {
            auto db = djinterop::engine::load_database(library.toStdString());
            auto tracks = db.tracks();
            if (tracks.size() != 1) {
                return false;
            }
            auto snapshot = tracks[0].snapshot();
            snapshot.bpm = bpm;
            snapshot.sample_rate = sampleRate > 0.0 ? std::optional<double>(sampleRate) : std::nullopt;
            snapshot.duration = std::chrono::milliseconds(60000);
            snapshot.beatgrid = {{-4, -4 * 22050.0}, {124, 124 * 22050.0}};
            tracks[0].update(snapshot);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    // Gives the track a waveform of `entries` entries, every one at
    // `level` in all three bands (the zoomed popup, #104).
    Q_INVOKABLE bool setWaveform(const QString &library, int entries, int level)
    {
        try {
            auto db = djinterop::engine::load_database(library.toStdString());
            auto tracks = db.tracks();
            if (tracks.size() != 1) {
                return false;
            }
            const auto value = static_cast<std::uint8_t>(level);
            auto snapshot = tracks[0].snapshot();
            snapshot.sample_count = 60ULL * 44100ULL;
            snapshot.waveform = std::vector<djinterop::waveform_entry>(
                static_cast<size_t>(entries), djinterop::waveform_entry{{value, 255}, {value, 255}, {value, 255}});
            tracks[0].update(snapshot);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    // Puts `hex` into the row's overviewWaveFormData as it is, for a blob
    // libdjinterop must not be handed (#104 review): Database2/m.db, the
    // column on PerformanceData or, in a newer schema, on Track.
    Q_INVOKABLE bool setOverviewBlob(const QString &library, const QString &hex)
    {
        const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
        const std::string file = library.toStdString() + "/Database2/m.db";
        sqlite3 *db = nullptr;
        if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            return false;
        }
        bool done = false;
        for (const char *sql : {"UPDATE PerformanceData SET overviewWaveFormData = ?1;",
                                "UPDATE Track SET overviewWaveFormData = ?1;"}) {
            sqlite3_stmt *stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_blob(stmt, 1, bytes.constData(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
                done = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0;
            }
            sqlite3_finalize(stmt);
            if (done) {
                break;
            }
        }
        sqlite3_close(db);
        return done;
    }

private:
    QStringList m_roots;
};
