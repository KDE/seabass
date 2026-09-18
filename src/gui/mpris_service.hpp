// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

namespace seabass::gui
{

class PlaybackController;

// Seabass as a media player on the desktop's session bus (MPRIS 2,
// https://specifications.freedesktop.org/mpris-spec/latest/). This is how
// the keyboard's media keys reach a player on a Linux desktop: Plasma and
// GNOME take those keys for themselves and pass them on to whichever
// MPRIS player is active, so an app that only listens for key events
// never sees them. It also puts the playing track in the desktop's media
// applet and on the lock screen, which is the rest of "as normal".
//
// Owned by the PlaybackController it speaks for. Nothing is registered
// until start(); where there is no session bus it does nothing.
class MprisService : public QObject
{
    Q_OBJECT
public:
    explicit MprisService(PlaybackController *controller);
    ~MprisService() override;

    // False when there is no session bus to be on, or the name is taken
    // and so is the per-process fallback.
    bool start();
    QString serviceName() const { return m_serviceName; }

private:
    void announce(const QString &interface, const QVariantMap &changed);

    PlaybackController *m_controller;
    QString m_serviceName;
};

// org.mpris.MediaPlayer2: what the player is.
class MprisRootAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ no CONSTANT)
    Q_PROPERTY(bool CanRaise READ no CONSTANT)
    Q_PROPERTY(bool HasTrackList READ no CONSTANT)
    Q_PROPERTY(QString Identity READ identity CONSTANT)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry CONSTANT)
    Q_PROPERTY(QStringList SupportedUriSchemes READ none CONSTANT)
    Q_PROPERTY(QStringList SupportedMimeTypes READ none CONSTANT)
public:
    explicit MprisRootAdaptor(QObject *parent) : QDBusAbstractAdaptor(parent) {}
    bool no() const { return false; }
    QString identity() const { return QStringLiteral("Seabass"); }
    QString desktopEntry() const { return QStringLiteral("seabass"); }
    QStringList none() const { return {}; }
public slots:
    void Raise() {}
    void Quit() {}
};

// org.mpris.MediaPlayer2.Player: what it is doing, and the buttons.
class MprisPlayerAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(double Rate READ one WRITE ignoreRate)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ one CONSTANT)
    Q_PROPERTY(double MaximumRate READ one CONSTANT)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ hasTrack)
    Q_PROPERTY(bool CanPause READ hasTrack)
    Q_PROPERTY(bool CanSeek READ hasTrack)
    Q_PROPERTY(bool CanControl READ yes CONSTANT)
public:
    MprisPlayerAdaptor(QObject *parent, PlaybackController *controller);

    QString playbackStatus() const;
    QVariantMap metadata() const;
    double volume() const;
    void setVolume(double volume);
    qlonglong position() const;  // microseconds, as MPRIS counts
    bool canGoNext() const;
    bool canGoPrevious() const;
    bool hasTrack() const;
    bool yes() const { return true; }
    double one() const { return 1.0; }
    void ignoreRate(double) {}

public slots:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offsetMicroseconds);
    void SetPosition(const QDBusObjectPath &trackId, qlonglong positionMicroseconds);
    void OpenUri(const QString &) {}

signals:
    void Seeked(qlonglong positionMicroseconds);

private:
    PlaybackController *m_controller;
};

}  // namespace seabass::gui
