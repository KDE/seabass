// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QPointer>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>
#include <vector>

namespace seabass::gui
{

// One music file for the metronome window (`seabass --metronome`):
// decoded and analysed off the GUI thread, by the decoder and the
// analyser an import uses and with the tempo range Preferences sets
// (an import also rations memory and gives up on a file that is too
// long; this does neither). What comes out is the beats, which the
// window's robot jumps to, the loudness to draw, and how long the
// analysis took. It reads the file and writes nothing anywhere.
class MetronomeTrack : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // The file that was analysed and has beats: empty until then.
    Q_PROPERTY(QUrl source READ source NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    // Tempo, key, whether the tempo moves, how many beats.
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    // How long the analysis took, in words.
    Q_PROPERTY(QString timing READ timing NOTIFY changed)
    Q_PROPERTY(int beatCount READ beatCount NOTIFY changed)
    Q_PROPERTY(double lengthMs READ lengthMs NOTIFY changed)

public:
    explicit MetronomeTrack(QObject *parent = nullptr);
    ~MetronomeTrack() override;

    QUrl source() const { return m_source; }
    bool busy() const { return m_busy; }
    QString title() const { return m_title; }
    QString summary() const { return m_summary; }
    QString error() const { return m_error; }
    QString timing() const { return m_timing; }
    int beatCount() const { return static_cast<int>(m_beatsMs.size()); }
    double lengthMs() const;

    // Analyses `file` in place of whatever was there. A file dropped
    // while another is still analysed takes its place: the older
    // analysis is told to stop and its answer is not looked at.
    Q_INVOKABLE void load(const QUrl &file);
    // Where `ms` lies between two beats: 0 on a beat, up to 1 just
    // before the next. -1 before the first beat and from the last on.
    Q_INVOKABLE double phaseAt(double ms) const;
    // Which beat `ms` follows, counted from 0; -1 before the first.
    Q_INVOKABLE int beatAt(double ms) const;
    // The beats from `fromMs` to `toMs`, for drawing: each as its time
    // in ms followed by its number.
    Q_INVOKABLE QVariantList beatsBetween(double fromMs, double toMs) const;
    // How loud the track is from `fromMs` to `toMs`, in `columns`
    // columns: each column's loudest moment, 0 to 1, 0 outside the track.
    Q_INVOKABLE QVariantList levels(double fromMs, double toMs, int columns) const;

private:
    // What an analysis gives back.
    struct Result
    {
        std::vector<double> beatsMs;
        std::vector<float> levels;
        double bpm = 0.0;
        QString key;
        bool followsTempo = false;
        QString error;
        // Spent inside the analyser, and from start to end with the decoding.
        double analysisMs = 0.0;
        double wholeMs = 0.0;
    };

signals:
    void changed();
    // The file is analysed and has beats: time to play it.
    void ready();

private:
    static Result analyseFile(const QString &path, double lowBpm, double highBpm,
                              const std::shared_ptr<std::atomic<bool>> &stop);
    void take(const QUrl &file, const Result &result);

    // The analysis that runs: a watcher of its own for each, so one
    // that was replaced can never be taken for the file that replaced it.
    QPointer<QFutureWatcher<Result>> m_watcher;
    // Set to end the analysis that runs: one flag an analysis.
    std::shared_ptr<std::atomic<bool>> m_stop;
    QUrl m_source;
    bool m_busy = false;
    QString m_title;
    QString m_summary;
    QString m_error;
    QString m_timing;
    std::vector<double> m_beatsMs;
    std::vector<float> m_levels;
};

}  // namespace seabass::gui
