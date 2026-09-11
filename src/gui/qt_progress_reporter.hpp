// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>

#include "application/ports/progress_reporter.hpp"

namespace seabass::gui
{

// Bridges application::ProgressReporter to Qt signals, so a scan running on
// a background thread (see ScanController/DuplicatesController) can drive a
// real progress bar on the UI thread instead of a spinner that never
// actually gets painted -- the same port cli/ implements as an ASCII bar.
//
// Instances are meant to be created fresh per background task and kept
// alive for that task's whole duration via a shared_ptr captured into the
// task's lambda, independent of whatever controller started it -- so a
// controller (and its QML page) can be destroyed mid-scan without the
// background thread touching a dangling object. Signals are connected to
// the controller using it as the context object, so Qt automatically
// stops delivering them once the controller is gone.
class QtProgressReporter : public QObject, public application::ProgressReporter
{
    Q_OBJECT

public:
    using QObject::QObject;

    void start(const std::string &label, size_t total) override
    {
        emit started(QString::fromStdString(label), static_cast<int>(total));
    }
    void tick(size_t current) override { emit progressed(static_cast<int>(current)); }
    void finish() override { emit finishedScanning(); }
    void warn(const std::string &message) override { emit warningRaised(QString::fromStdString(message)); }

signals:
    void started(const QString &label, int total);
    void progressed(int current);
    void finishedScanning();
    void warningRaised(const QString &message);
};

}  // namespace seabass::gui
