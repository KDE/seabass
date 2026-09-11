// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>

namespace seabass::application
{

// Thrown by CancellationToken::throwIfCancelled() from deep inside a read
// (a catalog reader's per-track loop, a directory walk) so the whole
// operation unwinds at once; a controller catches it at the task boundary
// and reports "cancelled", never "failed".
class OperationCancelled : public std::runtime_error
{
public:
    OperationCancelled() : std::runtime_error("operation cancelled") {}
};

// Cooperative cancellation for long-running use cases (the stick backup
// reads tens of GB; the user must be able to stop it). Deliberately not a
// method on ProgressReporter: the reporter is an output channel with
// several implementations already, whereas cancellation is an input that
// the GUI flips from a thread that never touches the reporter. Copies
// share the flag, so a use case receives one by value and the caller keeps
// another copy to cancel() from.
//
// Checked between chunks, never mid-syscall -- "stops after the current
// file" is the contract callers may promise their users.
class CancellationToken
{
public:
    CancellationToken() : m_flag(std::make_shared<std::atomic_bool>(false)) {}

    // A token nobody holds a cancelling copy of -- for callers that have
    // no cancel affordance (CLI one-shots, tests).
    static CancellationToken none() { return CancellationToken(); }

    void cancel() { m_flag->store(true, std::memory_order_relaxed); }
    bool cancelled() const { return m_flag->load(std::memory_order_relaxed); }

    // For read paths, where there is nothing to keep consistent and
    // unwinding is the simplest way out. Write paths poll cancelled()
    // between items instead, so they stop only at a consistent point.
    void throwIfCancelled() const
    {
        if (cancelled()) {
            throw OperationCancelled();
        }
    }

private:
    std::shared_ptr<std::atomic_bool> m_flag;
};

}  // namespace seabass::application
