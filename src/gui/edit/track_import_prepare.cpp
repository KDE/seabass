// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/track_import_prepare.hpp"

#include <QCollator>
#include <QStorageInfo>
#include <QStringList>

#include <algorithm>
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
// read to its end.
bool sameBytes(const std::string &a, const std::string &b)
{
    std::ifstream left(longPathSafe(pathFromUtf8(a)), std::ios::binary);
    std::ifstream right(longPathSafe(pathFromUtf8(b)), std::ios::binary);
    if (!left || !right) {
        return false;
    }
    std::vector<char> one(1 << 16);
    std::vector<char> other(1 << 16);
    while (true) {
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
        return "is on the stick already, as " + onStick(entry.existingPath, root);
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

PreparedImport prepareTrackImport(const ImportRequest &request, const application::CancellationToken &cancel,
                                  const ImportProgressSink &progress)
{
    const auto report = [&](ImportProgress::Step step, std::size_t done, std::size_t total, const std::string &file) {
        if (progress) {
            progress({step, done, total, file});
        }
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
        report(ImportProgress::Step::Files, files.size(), sources.size(), nameOf(source));
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
    tools.sameBytes = sameBytes;
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
                prepared.notImported.push_back({source, left->second});
            }
        } else if (entry.verdict != domain::ImportVerdict::Import) {
            prepared.notImported.push_back({source, whyNot(entry, rootText)});
            if (entry.verdict == domain::ImportVerdict::PossibleDuplicate) {
                prepared.possibleDuplicates.push_back(source);
            }
        } else if (left != leftOut.end()) {
            prepared.notImported.push_back({source, left->second});
        } else {
            prepared.bytesToCopy += entry.offered.file.fileSizeBytes;
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
        for (std::size_t i = 0; i < imports.size() && i < prepared.staged.staged.size(); ++i) {
            cancel.throwIfCancelled();
            const std::string &source = prepared.staged.staged[i];
            report(ImportProgress::Step::Analysis, i, imports.size(), nameOf(source));
            if (auto analysis = request.analyser(source, cancel)) {
                ++prepared.analysed;
                prepared.withGrid += analysis->beats.empty() ? 0 : 1;
                imports[i]->setAnalysis(std::move(analysis));
            }
        }
    }
    return prepared;
}

}  // namespace seabass::gui
