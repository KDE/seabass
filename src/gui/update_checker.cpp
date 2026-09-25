// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/update_checker.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVector>

#include "gui/seabass_settings.hpp"
#include "seabass_version.hpp"

namespace seabass::gui
{

namespace
{
// Where the website publishes what it has released. A static file,
// fetched whole: see the class comment for what that does and does not
// tell the server.
const auto FeedUrl = QStringLiteral("https://vizzzion.org/seabass/releases.json");
const auto DownloadPage = QStringLiteral("https://vizzzion.org/seabass/get-it.html");

// A day. Both the "is it due" test at startup and the timer while the
// app is running, so there is one number rather than two that could
// drift apart.
constexpr qint64 DaySeconds = 24 * 60 * 60;

const auto AutomaticKey = QStringLiteral("updates/automatic");
const auto LastCheckedKey = QStringLiteral("updates/lastChecked");
// Whether alphas and betas count here, and whether the checkbox for
// that is shown. See UpdateChecker::includeTesting.
const auto IncludeTestingKey = QStringLiteral("updates/includeTesting");
const auto TestingRevealedKey = QStringLiteral("updates/testingOptionRevealed");
}  // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_feedUrl(FeedUrl)
{
    QSettings settings = openSeabassSettings();
    m_automatic = settings.value(AutomaticKey, false).toBool();
    m_lastChecked = settings.value(LastCheckedKey).toDateTime();
    m_includeTesting = settings.value(IncludeTestingKey, false).toBool();
    m_testingOptionRevealed = settings.value(TestingRevealedKey, false).toBool();
    if (runningPreRelease() && !(m_includeTesting && m_testingOptionRevealed)) {
        // Running a pre-release is what opts a machine into hearing
        // about them, and the choice outlives this build: written now,
        // so that the stable release this one turns into still offers
        // the next alpha, with the checkbox there to say no.
        m_includeTesting = true;
        m_testingOptionRevealed = true;
        settings.setValue(IncludeTestingKey, true);
        settings.setValue(TestingRevealedKey, true);
    }

    m_daily.setInterval(DaySeconds * 1000);
    m_daily.setSingleShot(false);
    connect(&m_daily, &QTimer::timeout, this, &UpdateChecker::checkNow);
    if (m_automatic) {
        startTimer();
    }
}

UpdateChecker::~UpdateChecker() = default;

QString UpdateChecker::currentVersion() const
{
    return QString::fromLatin1(version::Number);
}

QString UpdateChecker::currentChannel() const
{
    return QString::fromLatin1(version::Channel);
}

QString UpdateChecker::currentCommit() const
{
    return QString::fromLatin1(version::Commit);
}

QString UpdateChecker::downloadPage() const
{
    return DownloadPage;
}

bool UpdateChecker::runningPreRelease() const
{
    return isPreReleaseChannel(currentChannel());
}

void UpdateChecker::setIncludeTesting(bool on)
{
    if (runningPreRelease()) {
        // A beta build follows every channel whatever the box says; the
        // Settings page shows the box ticked and disabled for it.
        on = true;
    }
    // Turning it off leaves the checkbox where it is: once found, the
    // option stays in view.
    rememberTesting(on, m_testingOptionRevealed || on);
}

bool UpdateChecker::versionTapped()
{
    if (!m_versionTaps.tap(QDateTime::currentMSecsSinceEpoch())) {
        return false;
    }
    rememberTesting(true, true);
    return true;
}

void UpdateChecker::forgetTestingChoice()
{
    rememberTesting(runningPreRelease(), runningPreRelease());
}

void UpdateChecker::rememberTesting(bool include, bool revealed)
{
    if (m_includeTesting == include && m_testingOptionRevealed == revealed) {
        return;
    }
    const bool policyChanged = m_includeTesting != include;
    m_includeTesting = include;
    m_testingOptionRevealed = revealed;
    QSettings settings = openSeabassSettings();
    settings.setValue(IncludeTestingKey, include);
    settings.setValue(TestingRevealedKey, revealed);
    Q_EMIT includeTestingChanged();
    if (policyChanged && m_haveFeed) {
        // The releases are known already; only which of them count
        // changed. Say so without another request.
        decide();
    }
}

void UpdateChecker::setAutomatic(bool on)
{
    if (m_automatic == on) {
        return;
    }
    m_automatic = on;
    QSettings settings = openSeabassSettings();
    settings.setValue(AutomaticKey, on);
    if (on) {
        // Explicitly enabled, so the daily timer starts now -- and the
        // first check happens straight away rather than in 24 hours,
        // because somebody who just turned this on wants an answer.
        startTimer();
        checkNow();
    } else {
        m_daily.stop();
    }
    Q_EMIT automaticChanged();
}

void UpdateChecker::startTimer()
{
    m_daily.start();
}

void UpdateChecker::checkIfDue()
{
    if (!m_automatic) {
        // The whole point of the default: with this off, Seabass makes
        // no network request of any kind.
        return;
    }
    if (m_lastChecked.isValid() && m_lastChecked.secsTo(QDateTime::currentDateTimeUtc()) < DaySeconds) {
        return;
    }
    checkNow();
}

void UpdateChecker::checkNow()
{
    if (m_state == QLatin1String("checking")) {
        return;
    }
    if (!version::isRelease()) {
        // A build from a working tree is not any published version.
        // Comparing it to one would either nag forever or claim it is up
        // to date, and both are lies.
        m_failure.clear();
        setState(QStringLiteral("notARelease"));
        return;
    }
    if (m_network == nullptr) {
        m_network = new QNetworkAccessManager(this);
    }
    setState(QStringLiteral("checking"));

    QNetworkRequest request{QUrl(m_feedUrl)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Named, so it is obvious in a server log who is asking and why, and
    // so nothing about this machine beyond the version rides along.
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Seabass/%1 (update check)").arg(currentVersion()));
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finish(reply); });
}

void UpdateChecker::finish(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        m_failure = reply->errorString();
        setState(QStringLiteral("failed"));
        return;
    }
    applyFeed(reply->readAll());
}

void UpdateChecker::applyFeed(const QByteArray &body)
{
    QJsonParseError problem{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &problem);
    if (problem.error != QJsonParseError::NoError || !document.isObject()) {
        m_failure = problem.errorString();
        setState(QStringLiteral("failed"));
        return;
    }

    QVector<ReleaseInfo> releases;
    const QJsonObject channels = document.object().value(QStringLiteral("channels")).toObject();
    for (auto channel = channels.begin(); channel != channels.end(); ++channel) {
        const QJsonArray list = channel.value().toArray();
        for (const QJsonValue &value : list) {
            const QJsonObject entry = value.toObject();
            ReleaseInfo release;
            release.version = entry.value(QStringLiteral("version")).toString();
            // The key is the channel; an entry that disagrees with the
            // list it is in would be a bug on the website, and the list
            // is the one that decides.
            release.channel = channel.key();
            release.build = entry.value(QStringLiteral("build")).toString();
            release.date = entry.value(QStringLiteral("date")).toString();
            // Absent counts as not released: a feed written by something
            // older than this field, or by hand, must not be able to
            // offer a build nobody has started.
            release.released = entry.value(QStringLiteral("released")).toBool();
            release.note = entry.value(QStringLiteral("note")).toString();
            release.noteLevel = entry.value(QStringLiteral("noteLevel")).toString();
            release.withdrawn = entry.value(QStringLiteral("withdrawn")).toBool();
            release.withdrawnReason = entry.value(QStringLiteral("withdrawnReason")).toString();
            if (!release.version.isEmpty()) {
                releases.append(release);
            }
        }
    }

    m_lastChecked = QDateTime::currentDateTimeUtc();
    QSettings settings = openSeabassSettings();
    settings.setValue(LastCheckedKey, m_lastChecked);

    m_releases = releases;
    m_haveFeed = true;
    decide();
}

void UpdateChecker::decide()
{
    const auto running = findRunning(currentVersion(), currentChannel(), m_releases);
    m_runningWithdrawn = running && running->withdrawn;
    m_runningWithdrawnReason = m_runningWithdrawn ? running->withdrawnReason : QString();

    const auto update = chooseUpdate(currentVersion(), currentChannel(), m_releases, m_includeTesting);
    if (update) {
        m_latest = *update;
        setState(QStringLiteral("updateAvailable"));
        return;
    }
    m_latest = {};
    // Withdrawn with nothing newer to move to is still worth saying: the
    // answer is "stop using this", not "you are up to date".
    setState(m_runningWithdrawn ? QStringLiteral("withdrawn") : QStringLiteral("upToDate"));
}

void UpdateChecker::setState(const QString &state)
{
    m_state = state;
    Q_EMIT stateChanged();
}

QString UpdateChecker::message() const
{
    if (m_state == QLatin1String("checking")) {
        return QStringLiteral("Checking…");
    }
    if (m_state == QLatin1String("failed")) {
        return QStringLiteral("Could not check: %1").arg(m_failure);
    }
    if (m_state == QLatin1String("notARelease")) {
        return QStringLiteral("This is a development build (%1), not a released version, so there is "
                              "nothing to compare it with.")
            .arg(currentCommit().isEmpty() ? currentVersion() : currentCommit());
    }
    if (m_state == QLatin1String("updateAvailable")) {
        QString text = QStringLiteral("Seabass %1 (%2) is available. You have %3.")
                           // What the package calls itself (alpha, beta,
                           // stable), which is what the user will see in
                           // its Settings, not the website list it is on.
                           .arg(m_latest.version, m_latest.build.isEmpty() ? m_latest.channel : m_latest.build,
                                currentVersion());
        if (m_runningWithdrawn) {
            text += QStringLiteral(" Your version has been withdrawn: %1").arg(m_runningWithdrawnReason);
        } else if (!m_latest.note.isEmpty()) {
            text += QLatin1Char(' ') + m_latest.note;
        }
        return text;
    }
    if (m_state == QLatin1String("withdrawn")) {
        return QStringLiteral("Your version has been withdrawn: %1 There is no newer release yet.")
            .arg(m_runningWithdrawnReason);
    }
    if (m_state == QLatin1String("upToDate")) {
        if (currentChannel() == QLatin1String("stable") && !m_includeTesting) {
            return QStringLiteral("Seabass %1 is the newest stable release.").arg(currentVersion());
        }
        return QStringLiteral("Seabass %1 is the newest release on any channel.").arg(currentVersion());
    }
    if (m_lastChecked.isValid()) {
        return QStringLiteral("Last checked %1.").arg(m_lastChecked.toLocalTime().toString(QStringLiteral("d MMM, HH:mm")));
    }
    return QStringLiteral("Not checked yet.");
}

}  // namespace seabass::gui
