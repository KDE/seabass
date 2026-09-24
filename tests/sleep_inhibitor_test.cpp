// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// SleepInhibitor: one system inhibitor however many operations overlap,
// let go with the last of them -- and the real one: on Linux, listed by
// systemd-inhibit while it is held; on Windows, granted by the system and,
// from an elevated prompt, listed by powercfg /requests.

#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QString>

#include <cassert>
#include <iostream>
#include <memory>
#include <optional>

#include "gui/sleep_inhibitor.hpp"

using seabass::gui::SleepInhibitor;

namespace
{

struct Counts
{
    int acquires = 0;
    int releases = 0;
    bool held = false;
    bool refuse = false;
    QString why;
};

class FakeSleep : public SleepInhibitor::Backend
{
public:
    explicit FakeSleep(std::shared_ptr<Counts> counts) : m_counts(std::move(counts)) {}
    bool acquire(const QString &why) override
    {
        ++m_counts->acquires;
        if (m_counts->refuse) {
            return false;
        }
        m_counts->held = true;
        m_counts->why = why;
        return true;
    }
    void release() override
    {
        ++m_counts->releases;
        m_counts->held = false;
    }

private:
    std::shared_ptr<Counts> m_counts;
};

#ifdef Q_OS_LINUX
// systemd-inhibit's own list, or nullopt when there is no logind to ask.
std::optional<QString> inhibitorList()
{
    if (QStandardPaths::findExecutable(QStringLiteral("systemd-inhibit")).isEmpty()) {
        return std::nullopt;
    }
    QProcess list;
    list.start(QStringLiteral("systemd-inhibit"), {QStringLiteral("--list"), QStringLiteral("--no-pager")});
    if (!list.waitForFinished(5000) || list.exitStatus() != QProcess::NormalExit || list.exitCode() != 0) {
        return std::nullopt;
    }
    return QString::fromUtf8(list.readAllStandardOutput());
}
#endif

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

#ifdef Q_OS_LINUX
    // Case 1, before any fake replaces it: the real backend, where logind
    // is there to ask. Skipped without one (containers, CI).
    {
        const auto before = inhibitorList();
        const QString why = QStringLiteral("sleep_inhibitor_test %1").arg(QCoreApplication::applicationPid());
        if (!before) {
            std::cout << "case 1 skipped: no logind to ask (systemd-inhibit --list unavailable)\n";
        } else {
            assert(!before->contains(why));
            {
                auto token = SleepInhibitor::hold(why);
                assert(SleepInhibitor::granted() && "logind must grant the inhibitor");
                const auto during = inhibitorList();
                assert(during && during->contains(why) && "logind must list the inhibitor while it is held");
            }
            const auto after = inhibitorList();
            assert(after && !after->contains(why) && "and drop it once the last hold is gone");
            std::cout << "case 1 (logind lists the inhibitor while held, and drops it after) OK\n";
        }
    }
#endif

#ifdef Q_OS_WIN
    // Case 1w, before any fake replaces it: the real power request. That
    // Windows grants it is checked everywhere; that powercfg /requests lists
    // it needs an elevated prompt, and is skipped without one (and under
    // Wine, which has no powercfg).
    {
        const QString why = QStringLiteral("sleep_inhibitor_test %1").arg(QCoreApplication::applicationPid());
        {
            auto token = SleepInhibitor::hold(why);
            assert(SleepInhibitor::granted() && "Windows must grant the power request");
            QProcess requests;
            requests.start(QStringLiteral("powercfg"), {QStringLiteral("/requests")});
            const bool finished = requests.waitForFinished(10000) && requests.exitStatus() == QProcess::NormalExit;
            const QString listed = finished ? QString::fromLocal8Bit(requests.readAllStandardOutput()) : QString();  // narrow-ok: powercfg console output, not a path
            // Gated on the listing itself, not the exit code: a prompt without
            // the rights to list requests may still exit 0 with a message.
            // A real listing always has its SYSTEM: section.
            if (listed.contains(QStringLiteral("SYSTEM:"))) {
                assert(listed.contains(why) && "powercfg /requests must list the request while it is held");
                std::cout << "case 1w: powercfg /requests lists the request while it is held\n";
            } else {
                std::cout << "case 1w: powercfg /requests not available here (needs an elevated prompt) -- "
                             "listing not checked\n";
            }
        }
        assert(!SleepInhibitor::granted());
        std::cout << "case 1w (Windows grants the power request, and it is let go after) OK\n";
    }
#endif

    auto counts = std::make_shared<Counts>();
    SleepInhibitor::setBackendForTesting(std::make_unique<FakeSleep>(counts));

    // Case 2: overlapping holds share one system inhibitor, named after the
    // first, and let it go with the last.
    {
        auto first = SleepInhibitor::hold(QStringLiteral("Backing up a USB stick"));
        auto second = SleepInhibitor::hold(QStringLiteral("Formatting a USB stick"));
        assert(counts->acquires == 1 && counts->held);
        assert(SleepInhibitor::granted());
        assert(counts->why == QStringLiteral("Backing up a USB stick"));
        assert(SleepInhibitor::activeHolds() == 2);
        first.reset();
        assert(counts->held && counts->releases == 0 && "one operation finishing must not wake the other's guard");
        second.reset();
        assert(!counts->held && counts->releases == 1);
        assert(!SleepInhibitor::granted() && "nothing is granted with nothing held");
        assert(SleepInhibitor::activeHolds() == 0);
        std::cout << "case 2 (overlapping holds share one inhibitor) OK\n";
    }

    // Case 3: a token copied into a task keeps the hold until the last copy
    // goes -- the way a QtConcurrent lambda carries it.
    {
        auto token = SleepInhibitor::hold(QStringLiteral("Saving changes to a DJ library"));
        auto copy = token;
        token.reset();
        assert(counts->held && "a copy of the token still holds");
        copy.reset();
        assert(!counts->held);
        std::cout << "case 3 (a copied token holds until the last copy goes) OK\n";
    }

    // Case 4: a system that refuses costs nothing: the token still works,
    // nothing is released that was never taken, and the next operation
    // after everything let go asks again.
    {
        counts->refuse = true;
        const int acquiresBefore = counts->acquires;
        const int releasesBefore = counts->releases;
        {
            auto token = SleepInhibitor::hold(QStringLiteral("Restoring a stick backup"));
            assert(SleepInhibitor::activeHolds() == 1);
            assert(!SleepInhibitor::granted() && "a refusal must not be reported as granted");
        }
        assert(counts->releases == releasesBefore && "a refused inhibitor must not be released");
        counts->refuse = false;
        {
            auto token = SleepInhibitor::hold(QStringLiteral("Restoring a stick backup"));
            assert(counts->acquires == acquiresBefore + 2 && counts->held && "the next hold asks again");
        }
        assert(!counts->held);
        std::cout << "case 4 (a refusal costs nothing and is asked again next time) OK\n";
    }

    SleepInhibitor::setBackendForTesting(nullptr);
    std::cout << "sleep_inhibitor_test: all cases passed\n";
    return 0;
}
