// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QDateTime>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QTimer>
#include <QVector>

#include "gui/update_decision.hpp"

class QNetworkAccessManager;
class QNetworkReply;

namespace seabass::gui
{

// Asks the website whether there is a newer Seabass, when the user has
// said it may.
//
// This is the only thing in Seabass that talks to the network, and the
// website promises no ads, no subscriptions and no phoning home. So the
// setting is off until somebody turns it on, and with it off nothing is
// ever sent: no check at startup, no timer, no request. "Check now"
// works either way, because pressing a button is the clearest consent
// there is.
//
// What goes out is an ordinary GET for a static file. There is no
// identifier, no version parameter, nothing about the libraries or
// sticks on this machine; the comparison happens here, after the whole
// file has arrived. The server learns what any web server learns from
// someone loading a page.
class UpdateChecker : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // Off by default, and persisted. Turning it on is what starts the
    // daily timer; turning it off stops it immediately.
    Q_PROPERTY(bool automatic READ automatic WRITE setAutomatic NOTIFY automaticChanged)

    // Whether alphas and betas count on this machine. A pre-release
    // build follows every channel whatever this says, and turns it on
    // for good the first time it runs, so that a beta user who moves to
    // the stable it became still hears about the next alpha. On a
    // stable build it starts off and hidden: the checkbox for it only
    // appears once testingOptionRevealed is true, which either running a
    // pre-release or the hidden tap sequence (versionTapped) makes so.
    Q_PROPERTY(bool includeTesting READ includeTesting WRITE setIncludeTesting NOTIFY includeTestingChanged)
    Q_PROPERTY(bool testingOptionRevealed READ testingOptionRevealed NOTIFY includeTestingChanged)
    // True for an alpha or beta build.
    Q_PROPERTY(bool runningPreRelease READ runningPreRelease CONSTANT)

    // idle, checking, upToDate, updateAvailable, withdrawn, failed, notARelease
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // One sentence for the Settings page, always suitable for showing.
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)

    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged)
    Q_PROPERTY(QString latestChannel READ latestChannel NOTIFY stateChanged)
    // What the website wants said about that release, if anything: "this
    // one fixes a bug that could lose cues", say. noteLevel is info or
    // warning.
    Q_PROPERTY(QString latestNote READ latestNote NOTIFY stateChanged)
    Q_PROPERTY(QString latestNoteLevel READ latestNoteLevel NOTIFY stateChanged)
    // Set when the running build has been withdrawn: the one thing worth
    // interrupting somebody for, because it means the build they are
    // using can hurt their library.
    Q_PROPERTY(bool runningWithdrawn READ runningWithdrawn NOTIFY stateChanged)
    Q_PROPERTY(QString runningWithdrawnReason READ runningWithdrawnReason NOTIFY stateChanged)

    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString currentChannel READ currentChannel CONSTANT)
    Q_PROPERTY(QString currentCommit READ currentCommit CONSTANT)
    Q_PROPERTY(QString downloadPage READ downloadPage CONSTANT)
    Q_PROPERTY(QDateTime lastChecked READ lastChecked NOTIFY stateChanged)

public:
    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    bool automatic() const { return m_automatic; }
    void setAutomatic(bool on);
    bool includeTesting() const { return m_includeTesting; }
    void setIncludeTesting(bool on);
    bool testingOptionRevealed() const { return m_testingOptionRevealed; }
    bool runningPreRelease() const;

    // One tap on the version line in Settings. The tenth within five
    // seconds switches includeTesting on and reveals its checkbox, on a
    // stable build that was not following test builds yet, and returns
    // true then only, so the page can say what just happened.
    Q_INVOKABLE bool versionTapped();
    // Back to the way a fresh stable install is: testing off and the
    // checkbox hidden again. A pre-release build stays on, as always.
    Q_INVOKABLE void forgetTestingChoice();

    QString state() const { return m_state; }
    QString message() const;
    bool updateAvailable() const { return m_state == QLatin1String("updateAvailable"); }
    QString latestVersion() const { return m_latest.version; }
    QString latestChannel() const { return m_latest.channel; }
    QString latestNote() const { return m_latest.note; }
    QString latestNoteLevel() const { return m_latest.noteLevel; }
    bool runningWithdrawn() const { return m_runningWithdrawn; }
    QString runningWithdrawnReason() const { return m_runningWithdrawnReason; }
    QString currentVersion() const;
    QString currentChannel() const;
    QString currentCommit() const;
    QString downloadPage() const;
    QDateTime lastChecked() const { return m_lastChecked; }

    // The button. Checks whatever the setting says, and reports.
    Q_INVOKABLE void checkNow();

    // Called once at startup. Does nothing at all unless the setting is
    // on and the last check was more than a day ago.
    Q_INVOKABLE void checkIfDue();

    // Tests hand it a file:// or a local test server instead of the
    // website. Not exposed to QML: this is not a user setting.
    void setFeedUrl(const QString &url) { m_feedUrl = url; }

Q_SIGNALS:
    void automaticChanged();
    void includeTestingChanged();
    void stateChanged();

private:
    void startTimer();
    void finish(QNetworkReply *reply);
    void applyFeed(const QByteArray &body);
    // What to say about the releases last fetched, given the settings as
    // they are now. Run again when includeTesting changes, so the answer
    // on the Settings page follows the box the moment it is ticked.
    void decide();
    void setState(const QString &state);
    void rememberTesting(bool include, bool revealed);

    QNetworkAccessManager *m_network = nullptr;
    QTimer m_daily;
    QString m_feedUrl;
    QString m_state = QStringLiteral("idle");
    QString m_failure;
    ReleaseInfo m_latest;
    // Every release the last feed listed, so a settings change can be
    // decided on without fetching again. Empty until a feed arrived.
    QVector<ReleaseInfo> m_releases;
    bool m_haveFeed = false;
    TapSequence m_versionTaps;
    bool m_runningWithdrawn = false;
    QString m_runningWithdrawnReason;
    QDateTime m_lastChecked;
    bool m_automatic = false;
    bool m_includeTesting = false;
    bool m_testingOptionRevealed = false;
};

}  // namespace seabass::gui
