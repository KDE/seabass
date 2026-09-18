// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "mpris_service.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>

#include "playback_controller.hpp"

namespace seabass::gui
{

namespace
{
const QString kObjectPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");
// MPRIS wants every track named by an object path. There is one track at
// a time and no track list, so one fixed path says all there is to say.
const QString kTrackPath = QStringLiteral("/org/kde/seabass/track/current");
const QString kNoTrackPath = QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack");
}  // namespace

MprisService::MprisService(PlaybackController *controller) : QObject(controller), m_controller(controller)
{
    new MprisRootAdaptor(this);
    auto *player = new MprisPlayerAdaptor(this, controller);

    // Adaptor properties do not announce themselves: a desktop applet
    // only redraws on org.freedesktop.DBus.Properties.PropertiesChanged.
    connect(controller, &PlaybackController::playingChanged, this,
            [this, player] { announce(kPlayerInterface, {{QStringLiteral("PlaybackStatus"), player->playbackStatus()}}); });
    const auto trackChanged = [this, player] {
        announce(kPlayerInterface, {{QStringLiteral("Metadata"), player->metadata()},
                                    {QStringLiteral("PlaybackStatus"), player->playbackStatus()},
                                    {QStringLiteral("CanPlay"), player->hasTrack()},
                                    {QStringLiteral("CanPause"), player->hasTrack()},
                                    {QStringLiteral("CanSeek"), player->hasTrack()}});
    };
    connect(controller, &PlaybackController::trackChanged, this, trackChanged);
    // The length is only known once the file is open, after trackChanged.
    connect(controller, &PlaybackController::durationChanged, this,
            [this, player] { announce(kPlayerInterface, {{QStringLiteral("Metadata"), player->metadata()}}); });
    connect(controller, &PlaybackController::queueChanged, this, [this, player] {
        announce(kPlayerInterface, {{QStringLiteral("CanGoNext"), player->canGoNext()},
                                    {QStringLiteral("CanGoPrevious"), player->canGoPrevious()}});
    });
    connect(controller, &PlaybackController::volumeChanged, this,
            [this, player] { announce(kPlayerInterface, {{QStringLiteral("Volume"), player->volume()}}); });
}

MprisService::~MprisService()
{
    if (!m_serviceName.isEmpty()) {
        QDBusConnection::sessionBus().unregisterObject(kObjectPath);
        QDBusConnection::sessionBus().unregisterService(m_serviceName);
    }
}

bool MprisService::start()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return false;
    }
    if (!bus.registerObject(kObjectPath, this, QDBusConnection::ExportAdaptors)) {
        return false;
    }
    // The plain name for the one Seabass a user normally runs; the form
    // the spec gives for a second instance of the same player otherwise.
    const QStringList names = {
        QStringLiteral("org.mpris.MediaPlayer2.seabass"),
        QStringLiteral("org.mpris.MediaPlayer2.seabass.instance%1").arg(QCoreApplication::applicationPid()),
    };
    for (const QString &name : names) {
        if (bus.registerService(name)) {
            m_serviceName = name;
            return true;
        }
    }
    bus.unregisterObject(kObjectPath);
    return false;
}

void MprisService::announce(const QString &interface, const QVariantMap &changed)
{
    if (m_serviceName.isEmpty()) {
        return;
    }
    QDBusMessage signal = QDBusMessage::createSignal(kObjectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                     QStringLiteral("PropertiesChanged"));
    signal << interface << changed << QStringList();
    QDBusConnection::sessionBus().send(signal);
}

MprisPlayerAdaptor::MprisPlayerAdaptor(QObject *parent, PlaybackController *controller)
    : QDBusAbstractAdaptor(parent), m_controller(controller)
{
}

QString MprisPlayerAdaptor::playbackStatus() const
{
    if (!m_controller->hasTrack()) {
        return QStringLiteral("Stopped");
    }
    return m_controller->playing() ? QStringLiteral("Playing") : QStringLiteral("Paused");
}

QVariantMap MprisPlayerAdaptor::metadata() const
{
    QVariantMap map;
    if (!m_controller->hasTrack()) {
        map.insert(QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(kNoTrackPath)));
        return map;
    }
    map.insert(QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(kTrackPath)));
    map.insert(QStringLiteral("xesam:title"), m_controller->title());
    // A list: MPRIS has it that a track can have several artists.
    map.insert(QStringLiteral("xesam:artist"), QStringList{m_controller->artist()});
    if (m_controller->duration() > 0) {
        map.insert(QStringLiteral("mpris:length"), static_cast<qlonglong>(m_controller->duration()) * 1000);
    }
    if (!m_controller->artworkPath().isEmpty()) {
        map.insert(QStringLiteral("mpris:artUrl"), m_controller->artworkPath());
    }
    return map;
}

double MprisPlayerAdaptor::volume() const { return m_controller->volume(); }
void MprisPlayerAdaptor::setVolume(double volume) { m_controller->setVolume(volume); }
qlonglong MprisPlayerAdaptor::position() const { return static_cast<qlonglong>(m_controller->position()) * 1000; }
bool MprisPlayerAdaptor::canGoNext() const { return m_controller->hasNext(); }
bool MprisPlayerAdaptor::canGoPrevious() const { return m_controller->hasPrevious(); }
bool MprisPlayerAdaptor::hasTrack() const { return m_controller->hasTrack(); }

void MprisPlayerAdaptor::Next() { m_controller->next(); }
void MprisPlayerAdaptor::Previous() { m_controller->previous(); }
void MprisPlayerAdaptor::Pause() { m_controller->pause(); }
void MprisPlayerAdaptor::PlayPause() { m_controller->togglePlay(); }
void MprisPlayerAdaptor::Stop() { m_controller->stop(); }
void MprisPlayerAdaptor::Play() { m_controller->play(); }

void MprisPlayerAdaptor::Seek(qlonglong offsetMicroseconds)
{
    if (!m_controller->hasTrack()) {
        return;
    }
    const qint64 target = qBound<qint64>(0, m_controller->position() + offsetMicroseconds / 1000,
                                         qMax<qint64>(0, m_controller->duration() - 1));
    m_controller->seek(target);
    emit Seeked(target * 1000);
}

void MprisPlayerAdaptor::SetPosition(const QDBusObjectPath &trackId, qlonglong positionMicroseconds)
{
    // For another track than the one loaded, or past its end, the spec
    // says to do nothing: the caller's idea of what is playing is stale.
    const qint64 target = positionMicroseconds / 1000;
    if (!m_controller->hasTrack() || trackId.path() != kTrackPath || target < 0 || target > m_controller->duration()) {
        return;
    }
    m_controller->seek(target);
    emit Seeked(target * 1000);
}

}  // namespace seabass::gui
