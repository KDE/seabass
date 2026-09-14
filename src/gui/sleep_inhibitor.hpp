// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <memory>

namespace seabass::gui
{

// Keeps the computer from going to sleep on its own while Seabass writes a
// stick or makes a backup: a suspend in the middle of either leaves a
// half-written stick or a half-written archive, and a long backup is exactly
// the kind of wait a machine is left alone for.
//
// What it stops is sleep the system decides on -- the idle timeout, a
// suspend another program asks for. It does not stop the user: closing the
// lid still suspends on a default logind (LidSwitchIgnoreInhibited=yes), and
// a Windows power request covers idle sleep only.
//
// hold() hands out a token. The system is kept awake while any token
// lives, however many operations overlap, and let go when the last one
// goes. Capture the token in the task that does the work, so it lives
// exactly as long as the task does.
//
// Linux: logind's Inhibit("sleep", ..., "block"). logind keeps the
// inhibitor for as long as Seabass holds the file descriptor it returns, so
// a crash lets go of it too. Windows: a power request
// (PowerRequestSystemRequired). Anywhere else, or when neither can be
// reached, holding does nothing: no operation fails for want of it.
class SleepInhibitor
{
public:
    class Hold;
    using Token = std::shared_ptr<Hold>;

    // `why` is shown to the user by the system (systemd-inhibit --list,
    // powercfg /requests).
    static Token hold(const QString &why);

    // How many tokens are alive right now.
    static int activeHolds();

    // Whether the system actually granted the inhibitor behind the tokens
    // alive right now: false with none alive, or when it was refused.
    static bool granted();

    // What actually talks to the system; swapped out by tests.
    class Backend
    {
    public:
        virtual ~Backend() = default;
        virtual bool acquire(const QString &why) = 0;
        virtual void release() = 0;
    };
    // Only while nothing is held.
    static void setBackendForTesting(std::unique_ptr<Backend> backend);
};

class SleepInhibitor::Hold
{
public:
    ~Hold();
    Hold(const Hold &) = delete;
    Hold &operator=(const Hold &) = delete;

private:
    friend class SleepInhibitor;
    Hold() = default;
};

}  // namespace seabass::gui
