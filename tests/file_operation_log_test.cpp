// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The operation log is the on-stick record of what Seabass wrote to
// somebody's library: the thing that is read, without Seabass running,
// when working out what a save did. Two ways it can fail are silent by
// design, and both were untested.
//
//   - The log lives under <stick>/Seabass, and an ofstream will not
//     create a directory. A log that never opened drops every line and
//     says nothing.
//   - record() deliberately swallows a failed open, because a stick that
//     cannot be written must not fail the save that was trying to
//     describe itself. So nothing in the calling code can notice.
//
// Between them, the only place either can be caught is here.

#include <cassert>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/logging/file_operation_log.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::infrastructure::logging::FileOperationLog;

namespace
{

std::vector<std::string> linesOf(const fs::path &file)
{
    std::vector<std::string> lines;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

// "2026-09-22T17:45:03  message": an ISO timestamp to the second, two
// spaces, then the message exactly as it was handed over. Checked
// structurally rather than against a clock, which would be a race.
bool looksLikeARecordOf(const std::string &line, const std::string &message)
{
    if (line.size() < 21) {
        return false;
    }
    for (size_t i = 0; i < 19; ++i) {
        const char c = line[i];
        const bool expectedDigit = (i != 4 && i != 7 && i != 10 && i != 13 && i != 16);
        if (expectedDigit && std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    if (line[4] != '-' || line[7] != '-' || line[10] != 'T' || line[13] != ':' || line[16] != ':') {
        return false;
    }
    return line.substr(19) == "  " + message;
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "file_operation_log_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // The regression this was fixed for: the log's directory does not
    // exist yet, which is every first save to a stick that has never
    // seen Seabass. Before create_directories() was added here, every
    // line of that save's record went nowhere.
    {
        const fs::path logFile = root / "stick" / "Seabass" / "operations.log";
        assert(!fs::exists(logFile.parent_path()));
        FileOperationLog log(logFile.string());
        log.record("wrote export.pdb");

        assert(fs::exists(logFile));
        const auto lines = linesOf(logFile);
        assert(lines.size() == 1);
        assert(looksLikeARecordOf(lines[0], "wrote export.pdb"));
    }

    // Lines from one log object, in the order they were recorded. The
    // stream is opened once and kept, which is the whole point of the
    // object, so the second line must not be the first one overwritten.
    {
        const fs::path logFile = root / "ordered" / "operations.log";
        FileOperationLog log(logFile.string());
        log.record("first");
        log.record("second");
        log.record("third");

        const auto lines = linesOf(logFile);
        assert(lines.size() == 3);
        assert(looksLikeARecordOf(lines[0], "first"));
        assert(looksLikeARecordOf(lines[1], "second"));
        assert(looksLikeARecordOf(lines[2], "third"));
    }

    // A later run appends to what the earlier one left. This is a
    // forensic record of the stick's whole history, not of one session:
    // opening it std::ios::trunc would erase the save before last, and
    // nothing in the app would report that it had.
    {
        const fs::path logFile = root / "appending" / "operations.log";
        {
            FileOperationLog first(logFile.string());
            first.record("run one");
        }
        {
            FileOperationLog second(logFile.string());
            second.record("run two");
        }

        const auto lines = linesOf(logFile);
        assert(lines.size() == 2);
        assert(looksLikeARecordOf(lines[0], "run one"));
        assert(looksLikeARecordOf(lines[1], "run two"));
    }

    // Two logs open on the same file at once, which is two Seabass
    // processes with one stick between them. Both handles are in append
    // mode and every line is one write, so lines interleave but none is
    // torn: each of the six comes back whole and attributable.
    {
        const fs::path logFile = root / "interleaved" / "operations.log";
        FileOperationLog a(logFile.string());
        FileOperationLog b(logFile.string());
        for (int i = 0; i < 3; ++i) {
            a.record("from A " + std::to_string(i));
            b.record("from B " + std::to_string(i));
        }

        const auto lines = linesOf(logFile);
        assert(lines.size() == 6);
        for (int i = 0; i < 3; ++i) {
            assert(looksLikeARecordOf(lines[i * 2], "from A " + std::to_string(i)));
            assert(looksLikeARecordOf(lines[i * 2 + 1], "from B " + std::to_string(i)));
        }
    }

    // The log cannot be opened at all, because its parent is a file
    // rather than a directory. record() must return quietly: a stick
    // that cannot be written must not take the save down with it.
    //
    // Worth saying what this case can and cannot see. It pins the
    // observable contract -- three records, no throw, and nothing
    // scribbled on the file in the way. It does NOT pin the `if
    // (!m_out) return;` guard: deleting that guard leaves this green,
    // because writing to a stream that never opened only sets failbit.
    // The guard is there for the day someone enables exceptions on the
    // stream, and no test in this file would notice its removal today.
    {
        const fs::path blocker = root / "not-a-directory";
        std::ofstream(blocker) << "I am a file\n";
        const fs::path logFile = blocker / "Seabass" / "operations.log";

        FileOperationLog log(logFile.string());
        log.record("first attempt");
        log.record("second attempt");
        log.record("third attempt");

        assert(fs::is_regular_file(blocker));
        assert(linesOf(blocker).size() == 1);  // and it did not scribble on it
    }

    // A message carrying a newline is written as given, so it reads back
    // as two records. Nothing in Seabass passes one today; pinned here
    // so that if something starts to, this says what the log will do
    // with it rather than the log quietly becoming unreadable.
    {
        const fs::path logFile = root / "newline" / "operations.log";
        FileOperationLog log(logFile.string());
        log.record("two\nlines");

        const auto lines = linesOf(logFile);
        assert(lines.size() == 2);
        assert(looksLikeARecordOf(lines[0], "two"));
        assert(lines[1] == "lines");  // no timestamp: it is not a record of its own
    }

    fs::remove_all(root, ec);
    std::cout << "file_operation_log_test passed\n";
    return 0;
}
