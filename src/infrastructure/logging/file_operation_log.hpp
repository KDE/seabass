// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <fstream>
#include <string>

#include "application/ports/operation_log.hpp"

namespace seabass::infrastructure::logging
{

// What every log object writes before its first line: which Seabass is
// writing, "Seabass 1.1.0 (stable, v1.1.0-3-gabc1234) on Linux". The
// version, the channel the build came off, the commit where it is known,
// and the system. A save or a command opens its own log object (a command
// that writes two libraries opens two, and says it twice), so the log says
// for each of them which build did it: the way to tell what a stick's
// rows went through when a later build writes them differently.
std::string writerLine();

// Appends timestamped lines to a plain text log file.
//
// The stream is opened once, on the first line, and kept until this object
// goes away. It used to open, append and close for every single line,
// which is a directory-metadata round trip each time on the removable
// media this writes to, and a save writes one or two lines per item: a
// 200-item save paid it 400 times.
//
// Two processes appending to the same log still interleave safely. Each
// holds its own handle opened in append mode, and every line is written
// and flushed as one operation, so a line lands whole rather than being
// mixed into another's. Flushing per line rather than buffering is
// deliberate and not the part worth optimising: this file is a forensic
// record of what was written to somebody's library, and a crash must not
// take the last few lines of it with them.
class FileOperationLog : public application::OperationLog
{
public:
    explicit FileOperationLog(std::string logFilePath);

    void record(const std::string &message) override;

private:
    std::string m_logFilePath;
    std::ofstream m_out;
};

}  // namespace seabass::infrastructure::logging
