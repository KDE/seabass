// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/track_import_prepare.hpp"

#include <QCollator>
#include <QStorageInfo>
#include <QThread>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <map>

#include "application/path_key.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/ports/track_metadata_probe.hpp"
#include "application/ports/track_tag_reader.hpp"
#include "domain/track_import_plan.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_catalogs.hpp"
#include "infrastructure/cleanup/audio_file_walk.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#include "infrastructure/audio/taglib_tag_reader.hpp"
#endif

namespace seabass::gui
{

namespace
{

namespace fs = std::filesystem;
using infrastructure::longPathSafe;

// Whether two files hold the same bytes; false when either cannot be
// read to its end, and on a cancel (the caller asks the token after the
// plan). Two files of a few GB are read piece by piece, so a cancel is
// answered within one piece.
bool sameBytes(const std::string &a, const std::string &b, const application::CancellationToken &cancel)
{
    std::ifstream left(longPathSafe(pathFromUtf8(a)), std::ios::binary);
    std::ifstream right(longPathSafe(pathFromUtf8(b)), std::ios::binary);
    if (!left || !right) {
        return false;
    }
    std::vector<char> one(1 << 16);
    std::vector<char> other(1 << 16);
    while (true) {
        if (cancel.cancelled()) {
            return false;
        }
        left.read(one.data(), static_cast<std::streamsize>(one.size()));
        right.read(other.data(), static_cast<std::streamsize>(other.size()));
        if (left.bad() || right.bad() || left.gcount() != right.gcount()) {
            return false;
        }
        if (!std::equal(one.begin(), one.begin() + left.gcount(), other.begin())) {
            return false;
        }
        if (left.gcount() == 0) {
            return left.eof() && right.eof();
        }
    }
}

std::uint64_t sizeOf(const fs::path &file)
{
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(longPathSafe(file), ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

// The stick's own spelling of `file`, from its root, for a sentence.
std::string onStick(const std::string &file, const std::string &root)
{
    return file.size() > root.size() && file.compare(0, root.size(), root) == 0
               ? file.substr(root.size() + (file[root.size()] == '/' ? 1 : 0))
               : file;
}

std::string nameOf(const std::string &file)
{
    return pathToUtf8(pathFromUtf8(file).filename());
}

std::string whyNot(const domain::ImportPlanEntry &entry, const std::string &root)
{
    using domain::ImportVerdict;
    switch (entry.verdict) {
    case ImportVerdict::AlreadyOnStick:
        return entry.inLibrary ? "is on the stick already, as " + onStick(entry.existingPath, root)
                               : "is on the stick already, as " + onStick(entry.existingPath, root)
                + ", but in no library, so it was not added (putting files that are on the stick already "
                  "into the libraries is not supported yet)";
    case ImportVerdict::PossibleDuplicate:
        return "may be the track the stick has as " + onStick(entry.existingPath, root);
    case ImportVerdict::OfferedTwice:
        return "is the same file as " + nameOf(entry.existingPath);
    case ImportVerdict::UnsupportedFormat:
        return "is not a format every library on this stick lists";
    case ImportVerdict::Unreadable:
        return "could not be read as audio";
    case ImportVerdict::TooLargeForFilesystem:
        return "is larger than one file may be on this stick";
    case ImportVerdict::DoesNotFit:
        return "does not fit on the stick";
    case ImportVerdict::Import:
        break;
    }
    return {};
}

}  // namespace

std::uint64_t ImportBar::analysisUnits(std::uint64_t bytes, std::size_t files)
{
    constexpr std::uint64_t MiB = 1024 * 1024;
    return std::max<std::uint64_t>(bytes / MiB * AnalysisUnitsPerMiB + (bytes % MiB) * AnalysisUnitsPerMiB / MiB,
                                   static_cast<std::uint64_t>(files));
}

ImportBar::ImportBar(ImportProgressSink sink)
    : m_sink(std::move(sink))
{
}

void ImportBar::plan(const std::vector<std::pair<ImportProgress::Step, std::uint64_t>> &stretches)
{
    if (!m_stretches.empty()) {
        return;
    }
    m_stretches = stretches;
    m_total = 0;
    for (const auto &[step, units] : m_stretches) {
        m_total += units;
    }
    // A plan of nothing still has an end to reach.
    if (m_total == 0) {
        m_total = 1;
    }
}

std::uint64_t ImportBar::positionOf(const ImportProgress &at) const
{
    std::uint64_t start = 0;
    for (const auto &[step, units] : m_stretches) {
        if (step != at.step) {
            start += units;
            continue;
        }
        // How far into its stretch the step is, as a share.
        double share = 0.0;
        if (step == ImportProgress::Step::Analysis && at.totalBytes > 0) {
            share = static_cast<double>(at.doneBytes) / static_cast<double>(at.totalBytes);
        } else if ((step == ImportProgress::Step::Analysis || step == ImportProgress::Step::Files
                    || step == ImportProgress::Step::Check)
                   && at.total > 0) {
            share = static_cast<double>(at.done) / static_cast<double>(at.total);
        }
        share = std::clamp(share, 0.0, 1.0);
        // The check's stretch: the check itself, then the covers it
        // counts.
        if (step == ImportProgress::Step::Check && at.total > 0) {
            const std::uint64_t fixed = std::min(units, CheckUnits);
            return start + fixed + static_cast<std::uint64_t>(std::floor(share * static_cast<double>(units - fixed)));
        }
        return start + static_cast<std::uint64_t>(std::floor(share * static_cast<double>(units)));
    }
    // A step the plan does not have: where the bar is.
    return m_reached;
}

void ImportBar::report(ImportProgress at)
{
    if (m_total > 0) {
        // Only end() reaches the end: the last file of the analysis done
        // is not yet the work done.
        m_reached = std::max(m_reached, std::min(positionOf(at), m_total - 1));
    }
    at.barDone = m_reached;
    at.barTotal = m_total;
    if (m_sink) {
        m_sink(at);
    }
}

void ImportBar::end(ImportProgress::Step step)
{
    if (m_total == 0) {
        plan({});
    }
    m_reached = m_total;
    ImportProgress at;
    at.step = step;
    at.barDone = m_reached;
    at.barTotal = m_total;
    if (m_sink) {
        m_sink(at);
    }
}

ImportProgressSink ImportBar::sink()
{
    return [this](const ImportProgress &at) { report(at); };
}

std::vector<std::shared_ptr<const ImportAnalysis>> analyseFiles(const std::vector<std::string> &files,
                                                                const ImportAnalyser &analyser, int jobs,
                                                                const application::CancellationToken &cancel,
                                                                const ImportProgressSink &progress)
{
    const std::size_t count = files.size();
    std::vector<std::shared_ptr<const ImportAnalysis>> made(count);
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::mutex reporting;
    std::exception_ptr thrown;
    std::atomic<bool> failed{false};
    // Under `reporting`: the files the workers have in hand, and the one
    // last done.
    std::vector<std::string> inHand;
    std::string lastDone;
    // Each file weighs on the bar by its size.
    std::vector<std::uint64_t> sizes(count);
    std::uint64_t totalBytes = 0;
    for (std::size_t i = 0; i < count; ++i) {
        sizes[i] = sizeOf(pathFromUtf8(files[i]));
        totalBytes += sizes[i];
    }
    std::uint64_t doneBytes = 0;
    const auto report = [&](std::size_t doneNow) {
        if (progress) {
            ImportProgress at{ImportProgress::Step::Analysis, doneNow, count, lastDone, inHand};
            at.doneBytes = doneBytes;
            at.totalBytes = totalBytes;
            progress(at);
        }
    };
    const auto work = [&]() {
        for (;;) {
            const std::size_t index = next.fetch_add(1);
            // One that failed fails the run: the rest of the files are
            // not analysed for nothing.
            if (index >= count || cancel.cancelled() || failed.load()) {
                return;
            }
            // The report too: nothing may leave a thread's function.
            try {
                const std::string name = nameOf(files[index]);
                {
                    const std::lock_guard<std::mutex> lock(reporting);
                    inHand.push_back(name);
                    report(done.load());
                }
                made[index] = analyser(files[index], cancel);
                const std::lock_guard<std::mutex> lock(reporting);
                if (const auto at = std::find(inHand.begin(), inHand.end(), name); at != inHand.end()) {
                    inHand.erase(at);
                }
                lastDone = name;
                doneBytes += sizes[index];
                report(done.fetch_add(1) + 1);
            } catch (...) {
                failed.store(true);
                const std::lock_guard<std::mutex> lock(reporting);
                if (!thrown) {
                    thrown = std::current_exception();
                }
                return;
            }
        }
    };
    report(0);
    const int workers = static_cast<int>(std::min<std::size_t>(std::max(1, jobs), std::max<std::size_t>(count, 1)));
    if (workers <= 1) {
        work();
    } else {
        std::vector<std::unique_ptr<QThread>> threads;
        for (int w = 0; w < workers; ++w) {
            threads.emplace_back(QThread::create(work));
            threads.back()->start();
        }
        for (const auto &thread : threads) {
            thread->wait();
        }
    }
    if (thrown) {
        std::rethrow_exception(thrown);
    }
    cancel.throwIfCancelled();
    return made;
}

namespace
{

// prepareTrackImport()'s work, on its bar.
PreparedImport prepareTrackImportOn(const ImportRequest &request, const application::CancellationToken &cancel,
                                    ImportBar &bar)
{
    const auto report = [&](ImportProgress::Step step, std::size_t done, std::size_t total, const std::string &file,
                            std::vector<std::string> inHand = {}) {
        bar.report({step, done, total, file, std::move(inHand)});
    };
    report(ImportProgress::Step::Stick, 0, 0, {});
    PreparedImport prepared;
    const std::string libraryPath =
        request.rekordboxPath.isEmpty() ? request.enginePath.toStdString() : request.rekordboxPath.toStdString();
    if (libraryPath.empty()) {
        prepared.refusal = "There is no stick to import onto.";
        return prepared;
    }
    const fs::path root = pathFromUtf8(libraryPath).parent_path();
    const std::string rootText = pathToGenericUtf8(root);

    // The stick: its libraries, every row of them, every audio file.
    application::NullProgressReporter quiet;
    const StickCatalogRead read = readAllStickCatalogs(libraryPath, quiet, cancel);
    if (!read.failed.empty()) {
        std::string names;
        for (const std::string &name : read.failed) {
            names += (names.empty() ? "" : ", ") + name;
        }
        prepared.refusal = "This stick has a library that could not be read (" + names
                           + "). A track belongs in every library of a stick, so nothing is imported.";
        return prepared;
    }
    const std::vector<std::string> present = read.catalogs.present();
    if (present.empty()) {
        prepared.refusal = "This stick has no library to import tracks into.";
        return prepared;
    }

    ImportTarget target;
    target.stickRoot = pathToQString(root);
    target.coverReader = request.coverReader;
    target.analysisPlanned = static_cast<bool>(request.analyser);
    std::set<std::string> formats;
    for (const std::string &format : present) {
        formats.insert(format);
        target.libraries << QString::fromStdString(format);
        const QString path = QString::fromStdString(catalogPathForFormat(libraryPath, format));
        (format == "engine" ? target.enginePath : target.pioneerRoot) = path;
    }

    domain::ImportStick stick;
    stick.root = rootText;
    for (const auto *catalog : {&read.catalogs.rekordbox, &read.catalogs.engine, &read.catalogs.oneLibrary}) {
        if (*catalog) {
            stick.libraryTracks.insert(stick.libraryTracks.end(), (*catalog)->begin(), (*catalog)->end());
        }
    }
    for (const auto &file : infrastructure::cleanup::walkAudioFiles(rootText, cancel).files) {
        stick.files.push_back({file.filePath, file.fileSizeBytes});
    }
    cancel.throwIfCancelled();
    const QStorageInfo storage(target.stickRoot);
    stick.freeBytes = storage.isValid() && storage.bytesAvailable() > 0 ? static_cast<std::uint64_t>(storage.bytesAvailable()) : 0;
    stick.clusterBytes = storage.isValid() && storage.blockSize() > 0 ? static_cast<std::uint64_t>(storage.blockSize()) : 0;
    stick.maxFileBytes = domain::maxFileBytesFor(QString::fromLatin1(storage.fileSystemType()).toLower().toStdString());
    // The save copies each library before it writes it, and may write a
    // copy beside it: twice their size, and a little for what grows.
    std::uint64_t libraries = 0;
    if (!target.pioneerRoot.isEmpty()) {
        const std::string pioneer = target.pioneerRoot.toStdString();
        libraries += sizeOf(pathFromUtf8(pioneer) / "rekordbox" / "export.pdb");
        libraries += sizeOf(pathFromUtf8(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer)));
    }
    if (!target.enginePath.isEmpty()) {
        libraries += sizeOf(pathFromUtf8(target.enginePath.toStdString()) / "Database2" / "m.db");
    }
    stick.reserveBytes = 2 * libraries + 32ull * 1024 * 1024;
    // And for each track, what the save writes beside its audio file.
    stick.sideWrites = domain::importSideWrites(formats, static_cast<bool>(request.analyser),
                                                static_cast<bool>(request.coverReader));
    stick.acceptedExtensions = domain::importableExtensions(formats);

    // What was offered, a folder as its audio files.
    std::vector<std::string> sources;
    for (const std::string &offered : request.offered) {
        std::error_code ec;
        if (fs::is_directory(longPathSafe(pathFromUtf8(offered)), ec)) {
            auto walked = infrastructure::cleanup::walkAudioFiles(offered, cancel).files;
            // As a file manager lists a folder: by name without regard
            // to case, "2" before "10", each level of folders on its own.
            QCollator collator;
            collator.setNumericMode(true);
            collator.setCaseSensitivity(Qt::CaseInsensitive);
            std::vector<std::pair<QStringList, std::string>> keyed;
            for (const auto &file : walked) {
                keyed.emplace_back(QString::fromStdString(file.filePath).split(QLatin1Char('/')), file.filePath);
            }
            std::stable_sort(keyed.begin(), keyed.end(), [&](const auto &a, const auto &b) {
                const qsizetype shared = std::min(a.first.size(), b.first.size());
                for (qsizetype i = 0; i < shared; ++i) {
                    if (const int order = collator.compare(a.first[i], b.first[i]); order != 0) {
                        return order < 0;
                    }
                }
                return a.first.size() < b.first.size() || (a.first.size() == b.first.size() && a.second < b.second);
            });
            for (size_t i = 0; i < walked.size(); ++i) {
                walked[i].filePath = keyed[i].second;
            }
            for (const auto &file : walked) {
                sources.push_back(file.filePath);
            }
        } else {
            sources.push_back(offered);
        }
    }
    // What the person took out of the batch is not offered again.
    std::erase_if(sources, [&](const std::string &source) { return request.leftOutByPerson.count(source) > 0; });

    // The work is known now: the bar's plan. The analysis is planned for
    // every file offered; the ones that are not staged are not analysed,
    // and the files that are move the bar the further each.
    std::uint64_t offeredBytes = 0;
    for (const std::string &source : sources) {
        offeredBytes += sizeOf(pathFromUtf8(source));
    }
    std::vector<std::pair<ImportProgress::Step, std::uint64_t>> stretches{
        {ImportProgress::Step::Stick, ImportBar::StickUnits},
        {ImportProgress::Step::Files, ImportBar::UnitsPerFileRead * sources.size()},
        {ImportProgress::Step::Check,
         ImportBar::CheckUnits + (request.coverReader ? ImportBar::UnitsPerCoverRead * sources.size() : 0)}};
    if (request.analyser) {
        stretches.emplace_back(ImportProgress::Step::Analysis, ImportBar::analysisUnits(offeredBytes, sources.size()));
    }
    bar.plan(stretches);

#ifdef SEABASS_HAVE_TAGLIB
    infrastructure::audio::TagLibMetadataProbe probe;
    infrastructure::audio::TagLibTagReader tags;
#else
    application::NullTrackMetadataProbe probe;
    application::NullTrackTagReader tags;
#endif
    std::vector<domain::ImportFile> files;
    for (const std::string &source : sources) {
        cancel.throwIfCancelled();
        report(ImportProgress::Step::Files, files.size(), sources.size(), nameOf(source), {nameOf(source)});
        domain::ImportFile file;
        file.file.format = "disk";
        file.file.sourceId = source;
        file.file.filePath = source;
        file.file.filename = nameOf(source);
        file.file.fileSizeBytes = sizeOf(pathFromUtf8(source));
        if (const auto metadata = probe.read(source)) {
            file.readable = true;
            file.file.title = metadata->title;
            file.file.artist = metadata->artist;
            file.file.album = metadata->album;
            file.file.durationSeconds = metadata->durationSeconds;
        }
        files.push_back(std::move(file));
    }

    report(ImportProgress::Step::Check, 0, 0, {});
    domain::ImportPlanTools tools;
    tools.pathKey = [](const std::string &path) { return application::normalizedPathKey(path); };
    tools.sameBytes = [&cancel](const std::string &a, const std::string &b) { return sameBytes(a, b, cancel); };
    domain::ImportChoices choices;
    choices.importAnyway = request.importAnyway;
    const domain::ImportPlan plan = planTrackImport(files, stick, tools, choices);
    cancel.throwIfCancelled();

    // What each library says of a file the stick has, for the files
    // offered that are on it already.
    struct Listed
    {
        OnStickTrack track;
        std::map<std::string, std::set<std::string>> playlistsIn;  // by library
    };
    std::map<std::string, Listed> listed;
    const std::pair<const char *, const std::optional<std::vector<domain::Track>> *> byLibrary[] = {
        {"rekordbox", &read.catalogs.rekordbox}, {"onelibrary", &read.catalogs.oneLibrary}, {"engine", &read.catalogs.engine}};
    for (const auto &[library, catalog] : byLibrary) {
        if (!*catalog) {
            continue;
        }
        for (const domain::Track &row : **catalog) {
            Listed &known = listed[application::normalizedPathKey(row.filePath)];
            if (!known.track.listedIn.contains(QLatin1String(library))) {
                known.track.listedIn << QLatin1String(library);
            } else if (!known.track.severalRowsIn.contains(QLatin1String(library))) {
                known.track.severalRowsIn << QLatin1String(library);
            }
            if (known.track.title.isEmpty()) {
                known.track.title = QString::fromStdString(row.title);
            }
            for (const auto &membership : row.playlists) {
                known.playlistsIn[library].insert(membership.name);
            }
        }
    }
    const OnStickLookup onStick = [&](const std::string &file, const std::string &playlist) {
        const auto known = listed.find(application::normalizedPathKey(file));
        if (known == listed.end()) {
            return OnStickTrack{};
        }
        OnStickTrack track = known->second.track;
        for (const auto &[library, playlists] : known->second.playlistsIn) {
            if (playlists.count(playlist) > 0) {
                track.inPlaylist << QString::fromStdString(library);
            }
        }
        return track;
    };

    prepared.staged = stageTrackImport(plan, target, tags, probe, request.date, request.playlist, onStick);
    if (!prepared.staged.refusal.empty()) {
        prepared.refusal = prepared.staged.refusal;
        return prepared;
    }
    std::map<std::string, std::string> leftOut;
    for (const LeftOut &left : prepared.staged.leftOut) {
        leftOut[left.sourceFile] = left.why;
    }
    for (const domain::ImportPlanEntry &entry : plan.entries) {
        const std::string &source = entry.offered.file.filePath;
        const auto left = leftOut.find(source);
        if (entry.verdict == domain::ImportVerdict::AlreadyOnStick
            && (left != leftOut.end()
                || std::find(prepared.staged.addedToPlaylist.begin(), prepared.staged.addedToPlaylist.end(), source)
                       != prepared.staged.addedToPlaylist.end())) {
            // Staged into the playlist, or left out of it with a reason
            // of its own.
            if (left != leftOut.end()) {
                prepared.notImported.push_back({source, left->second, true});
            }
        } else if (entry.verdict != domain::ImportVerdict::Import) {
            prepared.notImported.push_back(
                {source, whyNot(entry, rootText), entry.verdict == domain::ImportVerdict::AlreadyOnStick});
            if (entry.verdict == domain::ImportVerdict::PossibleDuplicate) {
                prepared.possibleDuplicates.push_back(source);
            }
        } else if (left != leftOut.end()) {
            prepared.notImported.push_back({source, left->second});
        } else {
            prepared.bytesToCopy += entry.offered.file.fileSizeBytes;
        }
    }
    // Each staged track's cover, small, for its row in the track list
    // until Save (which reads it again, at its own sizes, for the rows).
    if (request.coverReader) {
        std::vector<std::shared_ptr<ImportTrackChange>> withCover;
        for (const auto &change : prepared.staged.changes) {
            const auto import = std::dynamic_pointer_cast<ImportTrackChange>(change);
            if (import && import->withCover()) {
                withCover.push_back(import);
            }
        }
        // On the bar, as the check's last stretch: a cover a track.
        for (std::size_t i = 0; i < withCover.size(); ++i) {
            cancel.throwIfCancelled();
            const auto &import = withCover[i];
            bar.report({ImportProgress::Step::Check, i, withCover.size(), nameOf(import->sourceFile()),
                        {nameOf(import->sourceFile())}});
            if (const std::optional<CoverPictures> pictures = request.coverReader(import->sourceFile());
                pictures && !pictures->small.empty()) {
                prepared.covers.emplace(import->id(), pictures->small);
            }
        }
        if (!withCover.empty()) {
            bar.report({ImportProgress::Step::Check, withCover.size(), withCover.size(), {}, {}});
        }
    }
    if (request.analyser) {
        std::vector<std::shared_ptr<ImportTrackChange>> imports;
        for (const auto &change : prepared.staged.changes) {
            if (auto import = std::dynamic_pointer_cast<ImportTrackChange>(change)) {
                imports.push_back(std::move(import));
            }
        }
        // prepared.staged.staged names their source files, in the same
        // order.
        const std::size_t count = std::min(imports.size(), prepared.staged.staged.size());
        const std::vector<std::string> files(prepared.staged.staged.begin(),
                                             prepared.staged.staged.begin() + static_cast<std::ptrdiff_t>(count));
        std::vector<std::shared_ptr<const ImportAnalysis>> made =
            analyseFiles(files, request.analyser, request.analysisJobs, cancel, bar.sink());
        for (std::size_t i = 0; i < count; ++i) {
            if (made[i]) {
                ++prepared.analysed;
                prepared.withGrid += made[i]->beats.empty() ? 0 : 1;
                if (made[i]->followsTempo && !made[i]->beats.empty()) {
                    prepared.followedGrids.push_back(nameOf(prepared.staged.staged[i]));
                }
                imports[i]->setAnalysis(std::move(made[i]));
            }
        }
    }
    return prepared;
}

}  // namespace

PreparedImport prepareTrackImport(const ImportRequest &request, const application::CancellationToken &cancel,
                                  const ImportProgressSink &progress)
{
    // One bar from the offer to what is staged.
    ImportBar bar(progress);
    PreparedImport prepared = prepareTrackImportOn(request, cancel, bar);
    // Refused or staged, the work is done: the bar at its end, as the
    // step it ended in.
    bar.end(request.analyser && !prepared.staged.staged.empty() ? ImportProgress::Step::Analysis
                                                               : ImportProgress::Step::Check);
    return prepared;
}

}  // namespace seabass::gui
