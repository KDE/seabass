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
// The question on the home page: whether it has been answered, and the
// days of use counted towards asking it. See askAboutAutomatic.
const auto AutomaticAnsweredKey = QStringLiteral("updates/automaticAnswered");
const auto UseDaysKey = QStringLiteral("updates/useDays");
const auto LastUseDayKey = QStringLiteral("updates/lastUseDay");
constexpr int HourMs = 60 * 60 * 1000;
// Whether alphas and betas count here, and whether the checkbox for
// that is shown. See UpdateChecker::includeTesting.
const auto IncludeTestingKey = QStringLiteral("updates/includeTesting");
const auto TestingRevealedKey = QStringLiteral("updates/testingOptionRevealed");
}  // namespace

RunningBuild RunningBuild::thisBuild()
{
    return RunningBuild{QString::fromLatin1(version::Number), QString::fromLatin1(version::Channel),
                        QString::fromLatin1(version::Commit), version::Published};
}

UpdateChecker::UpdateChecker(QObject *parent)
    : UpdateChecker(RunningBuild::thisBuild(), parent)
{
}

UpdateChecker::UpdateChecker(RunningBuild build, QObject *parent)
    : QObject(parent)
    , m_build(std::move(build))
    , m_feedUrl(FeedUrl)
{
    QSettings settings = openSeabassSettings();
    m_automatic = settings.value(AutomaticKey, false).toBool();
    m_lastChecked = settings.value(LastCheckedKey).toDateTime();
    // The setting's own key is only ever written by a change to it, so
    // a key that is there is an answer: the checkbox in Preferences was
    // set, in any Seabass that shares these settings, a build from a
    // working tree included.
    m_automaticAnswered = settings.value(AutomaticAnsweredKey, false).toBool() || settings.contains(AutomaticKey);
    m_useDays = UseDays{settings.value(UseDaysKey, 0).toInt(), settings.value(LastUseDayKey).toString()};
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

    // Counting stops mattering once the question is settled, so the timer
    // only runs while it could still be asked.
    countUseDay(QDate::currentDate());
    m_useDayTick.setInterval(HourMs);
    connect(&m_useDayTick, &QTimer::timeout, this, [this] { countUseDay(QDate::currentDate()); });
    if (m_build.isRelease() && !m_automatic && !m_automaticAnswered && m_useDays.count < UseDaysBeforeAsking) {
        m_useDayTick.start();
    }
}

UpdateChecker::~UpdateChecker() = default;

QString UpdateChecker::currentVersion() const
{
    return m_build.version;
}

QString UpdateChecker::currentChannel() const
{
    return m_build.channel;
}

QString UpdateChecker::currentCommit() const
{
    return m_build.commit;
}

QString UpdateChecker::downloadPage() const
{
    return DownloadPage;
}

bool UpdateChecker::runningPreRelease() const
{
    // A dev build from a working tree is no test build anybody was handed:
    // it must not opt the machine into test builds, and every stable
    // Seabass installed on it would inherit that.
    return m_build.isRelease() && isPreReleaseChannel(currentChannel());
}

void UpdateChecker::setIncludeTesting(bool on)
{
    if (runningPreRelease()) {
        // A beta or published dev build follows every channel whatever
        // the box says; the Settings page shows the box ticked and
        // disabled for it.
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
    // True only when this actually switched something on: the page shows
    // a popup saying so, and on a test or development build (already
    // following everything, or offered nothing) or one of a channel
    // nothing knows (offered nothing) that would be a claim about a
    // change that did not happen.
    if (!tapsWouldEnableTesting(currentChannel(), m_includeTesting)) {
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
    if (policyChanged && m_haveFeed && mayRedecideFrom(m_state)) {
        // The releases are known already; only which of them count
        // changed. Say so without another request (see mayRedecideFrom
        // for why only over a settled answer).
        decide();
    }
}

bool UpdateChecker::askAboutAutomatic() const
{
    return shouldAskAboutAutomaticChecks(m_build.isRelease(), m_automatic, m_automaticAnswered, m_useDays.count);
}

void UpdateChecker::countUseDay(const QDate &today)
{
    // Enough days is enough: past that the count decides nothing, so it
    // is not written again.
    if (!m_build.isRelease() || m_automatic || m_automaticAnswered || !today.isValid()
        || m_useDays.count >= UseDaysBeforeAsking) {
        return;
    }
    const UseDays counted = seabass::gui::countUseDay(m_useDays, today.toString(Qt::ISODate));
    if (counted.count == m_useDays.count) {
        return;
    }
    const bool askedBefore = askAboutAutomatic();
    m_useDays = counted;
    QSettings settings = openSeabassSettings();
    settings.setValue(UseDaysKey, m_useDays.count);
    settings.setValue(LastUseDayKey, m_useDays.lastDay);
    if (m_useDays.count >= UseDaysBeforeAsking) {
        m_useDayTick.stop();
    }
    if (askAboutAutomatic() != askedBefore) {
        Q_EMIT askAboutAutomaticChanged();
    }
}

void UpdateChecker::answerAutomaticQuestion(bool allow)
{
    // Remembered first, so that a no is kept even though nothing else
    // changes with it.
    rememberAnswered();
    setAutomatic(allow);
}

void UpdateChecker::rememberAnswered()
{
    // A build from a working tree never asks, so it records no answer
    // to a question. The checkbox changed there is still an answer for
    // the released Seabass that shares these settings: setAutomatic
    // writes the setting's key, and the constructor reads that as one.
    if (m_automaticAnswered || !m_build.isRelease()) {
        return;
    }
    const bool askedBefore = askAboutAutomatic();
    m_automaticAnswered = true;
    m_useDayTick.stop();
    QSettings settings = openSeabassSettings();
    settings.setValue(AutomaticAnsweredKey, true);
    if (askAboutAutomatic() != askedBefore) {
        Q_EMIT askAboutAutomaticChanged();
    }
}

void UpdateChecker::setAutomatic(bool on)
{
    if (m_automatic == on) {
        return;
    }
    // Somebody who changes the setting, here or in Preferences, has
    // answered the question on the home page.
    rememberAnswered();
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
    if (!m_build.isRelease()) {
        // A build from a working tree is not any published version.
        // Comparing it to one would either nag forever or claim it is up
        // to date, and both are lies. A dev package CI made is published,
        // and does check (RunningBuild::published).
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
    // Qt gives a request that names no language the system's own. The
    // feed is one file in one language, so this one asks for any, and the
    // language this computer is set to stays here.
    request.setRawHeader("Accept-Language", "*");
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
