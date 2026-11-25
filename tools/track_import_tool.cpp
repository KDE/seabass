// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The track import without the app: audio files from this computer onto
// a stick, or a folder laid out like one, exactly as Browse imports them
// (prepareTrackImport, then one save through runSaveLoop: copy, rows in
// every library the stick has, covers, Seabass's own analysis). For
// running the analyser's output through the writers over many files, and
// reading it back with the benchmark.
//
//   track_import_tool --stick <root> [--playlist NAME [--create]] [--jobs N]
//                     [--low BPM --high BPM] [--no-analysis] [--no-covers]
//                     [--tsv out.tsv] [--list FILE] [file or folder ...]
//
// --stick: the folder that holds PIONEER and/or "Engine Library". It is
//   WRITTEN TO. Nothing else may write it meanwhile: unlike the app this
//   takes no write lock.
// --list: a file with one path a line (a line's first tab-separated
//   field), added to the files named.
// --tsv: one line per file offered: status (imported, added, left-out),
//   source, place on the stick, tempo, beats, followed (0 or 1),
//   confidence, reason.
// --low/--high: the tempo range of the analysis (default 70 to 180).
//
// Exit code 0 when the save succeeded (or there was nothing to save), 1
// on a refusal or a failed save, 2 on a wrong call.

#include <QCoreApplication>
#include <QDate>
#include <QFileInfo>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "application/ports/progress_reporter.hpp"
#include "domain/seabass_grid_analyser.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/edit/track_import_prepare.hpp"
#include "gui/qt_path.hpp"
#include "gui/track_import_analysis.hpp"
#include "gui/track_import_cover.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

int usage()
{
    std::fprintf(stderr,
                 "usage: track_import_tool --stick <root> [--playlist NAME [--create]] [--jobs N]\n"
                 "                         [--low BPM --high BPM] [--no-analysis] [--no-covers]\n"
                 "                         [--tsv out.tsv] [--list FILE] [file or folder ...]\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Qt takes the environment's locale; the numbers of the TSV are for a
    // script, with a point.
    std::setlocale(LC_NUMERIC, "C");
    std::string stick, playlist, tsvPath;
    bool create = false, analyse = true, covers = true;
    int jobs = 4;
    domain::TempoRange range;
    std::vector<std::string> offered;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::exit(usage());
            }
            return argv[++i];
        };
        if (arg == "--stick") {
            stick = value();
        } else if (arg == "--playlist") {
            playlist = value();
        } else if (arg == "--create") {
            create = true;
        } else if (arg == "--no-analysis") {
            analyse = false;
        } else if (arg == "--no-covers") {
            covers = false;
        } else if (arg == "--jobs") {
            jobs = std::atoi(value().c_str());
        } else if (arg == "--low") {
            range.lowBpm = std::atof(value().c_str());
        } else if (arg == "--high") {
            range.highBpm = std::atof(value().c_str());
        } else if (arg == "--tsv") {
            tsvPath = value();
        } else if (arg == "--list") {
            std::ifstream list(pathFromUtf8(value()));
            if (!list) {
                std::fprintf(stderr, "cannot read the list\n");
                return 2;
            }
            for (std::string line; std::getline(list, line);) {
                line = line.substr(0, line.find('\t'));
                if (!line.empty()) {
                    offered.push_back(line);
                }
            }
        } else if (arg.rfind("--", 0) == 0) {
            return usage();
        } else {
            offered.push_back(arg);
        }
    }
    if (stick.empty() || offered.empty() || jobs < 1 || jobs > 32) {
        return usage();
    }

    const fs::path root = fs::absolute(pathFromUtf8(stick));
    gui::ImportRequest request;
    std::error_code ec;
    if (fs::is_directory(root / "PIONEER", ec)) {
        request.rekordboxPath = gui::pathToQString(root / "PIONEER");
    }
    if (fs::is_directory(root / "Engine Library", ec)) {
        request.enginePath = gui::pathToQString(root / "Engine Library");
    }
    if (request.rekordboxPath.isEmpty() && request.enginePath.isEmpty()) {
        std::fprintf(stderr, "%s holds neither PIONEER nor Engine Library\n", stick.c_str());
        return 1;
    }
    for (const std::string &file : offered) {
        request.offered.push_back(pathToUtf8(fs::absolute(pathFromUtf8(file))));
    }
    if (!playlist.empty()) {
        gui::ImportPlaylist into;
        into.path = playlist;
        into.create = create;
        request.playlist = into;
    }
    request.date = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")).toStdString();
    if (covers) {
        request.coverReader = gui::readCoverPictures;
    }
    if (analyse) {
        request.analyser = [range](const std::string &file, const application::CancellationToken &cancel) {
            return gui::analyseForImport(file, cancel, range);
        };
        request.analysisJobs = jobs;
    }

    const application::CancellationToken cancel;
    gui::PreparedImport prepared;
    try {
        prepared = gui::prepareTrackImport(request, cancel, [](const gui::ImportProgress &at) {
            if (at.step == gui::ImportProgress::Step::Analysis && at.total > 0 && (at.done % 25 == 0 || at.done == at.total)) {
                std::fprintf(stderr, "analysed %zu of %zu\n", at.done, at.total);
            }
        });
    } catch (const std::exception &e) {
        std::fprintf(stderr, "could not prepare the import: %s\n", e.what());
        return 1;
    }
    if (!prepared.refusal.empty()) {
        std::fprintf(stderr, "%s\n", prepared.refusal.c_str());
        return 1;
    }

    std::ofstream tsv;
    if (!tsvPath.empty()) {
        tsv.open(pathFromUtf8(tsvPath));
        tsv << "status\tsource\ton_stick\tbpm\tbeats\tfollowed\tconfidence\treason\n";
    }
    std::size_t at = 0;
    for (const auto &change : prepared.staged.changes) {
        const auto import = std::dynamic_pointer_cast<gui::ImportTrackChange>(change);
        if (!import || at >= prepared.staged.staged.size()) {
            continue;
        }
        const auto &analysis = import->analysis();
        if (tsv.is_open()) {
            char numbers[96];
            std::snprintf(numbers, sizeof numbers, "%.4f\t%zu\t%d\t%.3f", analysis ? analysis->bpm : 0.0,
                          analysis ? analysis->beats.size() : std::size_t{0}, analysis && analysis->followsTempo ? 1 : 0,
                          analysis ? analysis->confidence : 0.0);
            tsv << "imported\t" << prepared.staged.staged[at] << '\t' << import->destinationFile() << '\t' << numbers << "\t\n";
        }
        ++at;
    }
    if (tsv.is_open()) {
        for (const std::string &source : prepared.staged.addedToPlaylist) {
            tsv << "added\t" << source << "\t\t\t\t\t\t\n";
        }
        for (const gui::LeftOut &left : prepared.notImported) {
            tsv << "left-out\t" << left.sourceFile << "\t\t\t\t\t\t" << left.why << '\n';
        }
    }
    std::printf("%zu to import (%d analysed, %d with a grid, %zu with a followed grid), %zu already on the stick for the "
                "playlist, %zu left out, %.1f MB to copy\n",
                prepared.staged.staged.size(), prepared.analysed, prepared.withGrid, prepared.followedGrids.size(),
                prepared.staged.addedToPlaylist.size(), prepared.notImported.size(),
                static_cast<double>(prepared.bytesToCopy) / (1024.0 * 1024.0));
    if (!prepared.staged.note.empty()) {
        std::printf("%s\n", prepared.staged.note.c_str());
    }
    if (prepared.staged.changes.empty()) {
        return 0;
    }

    gui::SaveContext ctx(cancel, application::NullProgressReporter::instance(), nullptr, request.rekordboxPath,
                         request.enginePath);
    const gui::SaveLoopResult saved = gui::runSaveLoop(prepared.staged.changes, ctx);
    if (!saved.error.isEmpty()) {
        std::fprintf(stderr, "the save failed and was taken back: %s\n", saved.error.toStdString().c_str());
        return 1;
    }
    std::printf("saved: %lld change(s) written, %lld skipped\n", static_cast<long long>(saved.appliedIds.size()),
                static_cast<long long>(saved.skippedIds.size()));
    return 0;
}
