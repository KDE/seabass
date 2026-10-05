// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "onelibrary_cue_source_probe_lib.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QString>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/backup/interrupted_save.hpp"
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "infrastructure/sqlite_pending_journal.hpp"

namespace fs = std::filesystem;

namespace seabass::omnis59
{

using domain::CuePoint;
using infrastructure::onelibrary::OneLibraryCueWriter;
using infrastructure::onelibrary::SqlCipherDb;
using infrastructure::onelibrary::SqlCipherLibrary;
using infrastructure::onelibrary::SqlCipherStatement;
namespace rb = infrastructure::rekordbox;

namespace
{

constexpr uint32_t Pco2Fourcc = 0x50434f32;  // "PCO2"
constexpr uint32_t PcobFourcc = 0x50434f42;  // "PCOB"

std::string pioneerOf(const std::string &stickRoot)
{
    return pathToUtf8(pathFromUtf8(stickRoot) / "PIONEER");
}

std::string lower(std::string text)
{
    for (char &c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string mmss(uint32_t ms)
{
    const uint32_t tenths = (ms % 1000) / 100;
    std::ostringstream out;
    out << ms / 60000 << ':' << std::setw(2) << std::setfill('0') << (ms / 1000) % 60;
    if (tenths) {
        out << '.' << tenths;
    }
    return out.str();
}

char padLetter(uint32_t slot)
{
    return slot >= 1 && slot <= 8 ? static_cast<char>('A' + slot - 1) : '?';
}

std::string describe(const ListEntry &e)
{
    std::string text = e.slot ? std::string("pad ") + padLetter(e.slot) + " " : std::string("memory ");
    text += mmss(e.timeMs);
    if (e.isLoop) {
        text += "-" + mmss(e.loopEndMs) + " loop";
    }
    return text;
}

std::string describe(const std::vector<ListEntry> &list)
{
    if (list.empty()) {
        return "(empty)";
    }
    std::string text;
    for (const auto &e : list) {
        text += (text.empty() ? "" : ", ") + describe(e);
    }
    return text;
}

std::string describe(const TableCue &c)
{
    std::string text = c.kind ? std::string("pad ") + padLetter(static_cast<uint32_t>(c.kind)) + " "
                              : std::string("memory ");
    text += mmss(static_cast<uint32_t>(c.inUsec / 1000));
    if (c.isActiveLoop || c.outUsec != c.inUsec) {
        text += "-" + mmss(static_cast<uint32_t>(c.outUsec / 1000)) + (c.isActiveLoop ? " loop" : "");
    }
    if (!c.comment.empty()) {
        text += " \"" + c.comment + "\"";
    }
    return text;
}

std::string describe(const std::vector<TableCue> &rows)
{
    if (rows.empty()) {
        return "(no rows)";
    }
    std::string text;
    for (const auto &c : rows) {
        text += (text.empty() ? "" : ", ") + describe(c);
    }
    return text;
}

std::vector<ListEntry> sortedBySlotAndTime(std::vector<ListEntry> list)
{
    std::sort(list.begin(), list.end(), [](const ListEntry &a, const ListEntry &b) {
        return std::tie(a.slot, a.timeMs, a.loopEndMs) < std::tie(b.slot, b.timeMs, b.loopEndMs);
    });
    return list;
}

CuePoint hot(int slot, double ms, const std::string &comment = {})
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Hot;
    cue.hotCueNumber = slot;
    cue.positionMs = ms;
    cue.comment = comment;
    return cue;
}

CuePoint hotLoop(int slot, double fromMs, double toMs)
{
    CuePoint cue = hot(slot, fromMs);
    cue.isLoop = true;
    cue.loopEndMs = toMs;
    return cue;
}

// exportLibrary.db read from a copy on this computer, never opened on the
// stick: even a read-only SQLite open of a WAL database may create its
// -shm, and an open that may write rolls a hot journal back. The copy
// takes the -wal with it, so rows a player left in the log are read too.
class DbCopy
{
public:
    explicit DbCopy(const std::string &pioneerRoot)
    {
        const fs::path db = pathFromUtf8(OneLibraryCueWriter::dbPathFor(pioneerRoot));
        static int serial = 0;
        m_dir = fs::temp_directory_path()
            / pathFromUtf8("omnis59-db-" + std::to_string(QCoreApplication::applicationPid()) + "-" + std::to_string(++serial));
        fs::remove_all(m_dir);
        fs::create_directories(m_dir);
        fs::copy_file(db, m_dir / "exportLibrary.db");
        for (const char *suffix : {"-wal", "-journal"}) {
            fs::path side = db;
            side += suffix;
            std::error_code ec;
            if (fs::is_regular_file(side, ec) && fs::file_size(side, ec) > 0) {
                fs::copy_file(side, m_dir / pathFromUtf8(std::string("exportLibrary.db") + suffix));
            }
        }
        m_db = std::make_unique<SqlCipherDb>(m_lib, pathToUtf8(m_dir / "exportLibrary.db"), /*readOnly=*/false);
        m_db->exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
    }
    ~DbCopy()
    {
        m_db.reset();
        std::error_code ec;
        fs::remove_all(m_dir, ec);
    }
    SqlCipherDb &db() { return *m_db; }

private:
    fs::path m_dir;
    SqlCipherLibrary m_lib;
    std::unique_ptr<SqlCipherDb> m_db;
};

std::vector<std::string> columnsOf(SqlCipherDb &db, const std::string &table)
{
    std::vector<std::string> columns;
    SqlCipherStatement info(db, "PRAGMA table_info(\"" + table + "\")");
    while (info.step()) {
        columns.push_back(info.columnText(1));
    }
    return columns;
}

// Every row of `table` (optionally filtered), each as one line of SQL
// literals, in rowid order where there is one.
std::vector<std::string> rowsOf(SqlCipherDb &db, const std::string &table, const std::string &where = {},
                                const std::optional<int64_t> &bind = std::nullopt)
{
    const auto columns = columnsOf(db, table);
    std::string expr;
    for (const auto &column : columns) {
        expr += (expr.empty() ? "" : " || '|' || ") + std::string("quote(\"") + column + "\")";
    }
    std::vector<std::string> rows;
    if (expr.empty()) {
        return rows;
    }
    SqlCipherStatement select(db, "SELECT " + expr + " FROM \"" + table + "\"" + (where.empty() ? "" : " WHERE " + where));
    if (bind) {
        select.bindInt64(1, *bind);
    }
    while (select.step()) {
        rows.push_back(select.columnText(0));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

std::vector<int64_t> contentIdsAt(SqlCipherDb &db, const std::string &contentPath)
{
    std::vector<int64_t> ids;
    SqlCipherStatement find(db, "SELECT content_id FROM content WHERE path = ? ORDER BY content_id");
    find.bindText(1, contentPath);
    while (find.step()) {
        ids.push_back(find.columnInt64(0));
    }
    return ids;
}

std::vector<TableCue> cueRowsOf(SqlCipherDb &db, int64_t contentId)
{
    std::vector<TableCue> rows;
    SqlCipherStatement select(db,
                              "SELECT kind, inUsec, outUsec, isActiveLoop, cueComment FROM cue WHERE content_id = ? "
                              "ORDER BY kind, inUsec, outUsec");
    select.bindInt64(1, contentId);
    while (select.step()) {
        TableCue cue;
        cue.kind = select.columnInt64(0);
        cue.inUsec = select.columnInt64(1);
        cue.outUsec = select.columnInt64(2);
        cue.isActiveLoop = select.columnIsNull(3) ? 0 : select.columnInt64(3);
        cue.comment = select.columnIsNull(4) ? std::string() : select.columnText(4);
        rows.push_back(cue);
    }
    return rows;
}

std::string sha256Of(const fs::path &file, qint64 &size)
{
    QFile in(gui::pathToQString(file));
    if (!in.open(QIODevice::ReadOnly)) {
        size = -1;
        return "unreadable";
    }
    size = in.size();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&in);
    return hash.result().toHex().toStdString();
}

QJsonArray toJson(const std::vector<ListEntry> &list)
{
    QJsonArray array;
    for (const auto &e : list) {
        array.append(QJsonObject{{"slot", int(e.slot)},
                                 {"timeMs", double(e.timeMs)},
                                 {"isLoop", e.isLoop},
                                 {"loopEndMs", double(e.loopEndMs)},
                                 {"text", QString::fromStdString(describe(e))}});
    }
    return array;
}

std::vector<ListEntry> listFromJson(const QJsonArray &array)
{
    std::vector<ListEntry> list;
    for (const auto &value : array) {
        const QJsonObject o = value.toObject();
        list.push_back({static_cast<uint32_t>(o["slot"].toInt()), static_cast<uint32_t>(o["timeMs"].toDouble()),
                        o["isLoop"].toBool(), static_cast<uint32_t>(o["loopEndMs"].toDouble())});
    }
    return list;
}

QJsonArray toJson(const std::vector<TableCue> &rows)
{
    QJsonArray array;
    for (const auto &c : rows) {
        array.append(QJsonObject{{"kind", double(c.kind)},
                                 {"inUsec", double(c.inUsec)},
                                 {"outUsec", double(c.outUsec)},
                                 {"isActiveLoop", double(c.isActiveLoop)},
                                 {"comment", QString::fromStdString(c.comment)},
                                 {"text", QString::fromStdString(describe(c))}});
    }
    return array;
}

QJsonArray toJson(const std::vector<std::string> &lines)
{
    QJsonArray array;
    for (const auto &line : lines) {
        array.append(QString::fromStdString(line));
    }
    return array;
}

std::vector<std::string> stringsFromJson(const QJsonArray &array)
{
    std::vector<std::string> out;
    for (const auto &value : array) {
        out.push_back(value.toString().toStdString());
    }
    return out;
}

const std::vector<std::pair<const char *, std::vector<ListEntry> AnalysisLists::*>> &listNames()
{
    static const std::vector<std::pair<const char *, std::vector<ListEntry> AnalysisLists::*>> names{
        {"pco2Hot", &AnalysisLists::pco2Hot},
        {"pco2Memory", &AnalysisLists::pco2Memory},
        {"extPcobHot", &AnalysisLists::extPcobHot},
        {"extPcobMemory", &AnalysisLists::extPcobMemory},
        {"datPcobHot", &AnalysisLists::datPcobHot},
        {"datPcobMemory", &AnalysisLists::datPcobMemory},
    };
    return names;
}

QJsonObject toJson(const AnalysisLists &lists)
{
    QJsonObject o;
    o["extPresent"] = lists.extPresent;
    o["datPresent"] = lists.datPresent;
    for (const auto &[name, member] : listNames()) {
        o[name] = toJson(lists.*member);
    }
    o["problems"] = toJson(lists.problems);
    return o;
}

// Keeps only pad A in the .DAT's legacy hot list, through the PCOB codec
// and AnlzFile's checked atomic write, then the writer's own read-back
// check; puts the file back if that check fails.
void trimLegacyHotListToPadA(const std::string &pioneerRoot, const std::string &analysisFile)
{
    const std::string datPath = rb::datAnlzPath(pioneerRoot, analysisFile);
    auto dat = rb::AnlzFile::readRaw(datPath);
    const std::string before = dat.toBytes();
    bool found = false;
    for (auto &section : dat.sections) {
        if (section.fourcc != PcobFourcc || section.rawBytes.size() < 16
            || rb::readU32BE(section.rawBytes, 12) != rb::CueListTypeHot) {
            continue;
        }
        found = true;
        std::vector<rb::LegacyCueEntry> keep;
        for (const auto &entry : rb::AnlzLegacyCueCodec::decodeCues(section.rawBytes)) {
            if (entry.hotCueNumber == 1) {
                keep.push_back(entry);
            }
        }
        section.rawBytes = rb::AnlzLegacyCueCodec::encodeCues(keep, rb::CueListTypeHot,
                                                               rb::AnlzLegacyCueCodec::memoryCountOf(section.rawBytes));
    }
    if (!found) {
        throw std::runtime_error(datPath + " has no legacy hot cue list to trim");
    }
    const std::string after = dat.toBytes();
    dat.writeRaw(datPath);
    if (auto problem = rb::analysisFileReadBackProblem(datPath, after)) {
        const bool restored = infrastructure::writeFileDurablyAtomic(datPath, before);
        throw std::runtime_error(datPath + " failed its read-back after the PCOB trim (" + *problem + "); "
                                 + (restored ? "put back as it was" : "could NOT be put back"));
    }
}

std::vector<std::string> allTables(SqlCipherDb &db)
{
    std::vector<std::string> names;
    SqlCipherStatement list(db, "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name");
    while (list.step()) {
        names.push_back(list.columnText(0));
    }
    return names;
}

}  // namespace

AnalysisLists readAnalysisLists(const std::string &pioneerRoot, const std::string &analyzePath)
{
    AnalysisLists lists;
    const auto readFile = [&](const std::string &path, bool isExt) {
        std::error_code ec;
        if (!fs::is_regular_file(pathFromUtf8(path), ec)) {
            if (isExt) {
                lists.problems.push_back(path + " is not there");
            }
            return;
        }
        (isExt ? lists.extPresent : lists.datPresent) = true;
        rb::AnlzFile file;
        try {
            file = rb::AnlzFile::readRaw(path);
        } catch (const std::exception &e) {
            lists.problems.push_back(path + ": " + e.what());
            return;
        }
        for (const auto &section : file.sections) {
            if (section.rawBytes.size() < 16) {
                continue;
            }
            const uint32_t type = rb::readU32BE(section.rawBytes, 12);
            try {
                if (section.fourcc == Pco2Fourcc) {
                    if (!isExt) {
                        lists.problems.push_back(path + " holds a PCO2 list");
                    }
                    auto &target = type == rb::CueListTypeHot ? lists.pco2Hot : lists.pco2Memory;
                    for (const auto &e : rb::AnlzCueCodec::decodeHotCues(section.rawBytes, type)) {
                        target.push_back({e.hotCueNumber, e.timeMs, e.isLoop, e.loopEndMs});
                    }
                } else if (section.fourcc == PcobFourcc) {
                    auto &target = type == rb::CueListTypeHot ? (isExt ? lists.extPcobHot : lists.datPcobHot)
                                                              : (isExt ? lists.extPcobMemory : lists.datPcobMemory);
                    for (const auto &e : rb::AnlzLegacyCueCodec::decodeCues(section.rawBytes)) {
                        target.push_back({e.hotCueNumber, e.timeMs, e.isLoop, e.loopEndMs});
                    }
                }
            } catch (const std::exception &e) {
                lists.problems.push_back(path + ": " + e.what());
            }
        }
    };
    readFile(rb::extAnlzPath(pioneerRoot, analyzePath), true);
    readFile(rb::datAnlzPath(pioneerRoot, analyzePath), false);
    return lists;
}

std::vector<Candidate> findCandidates(const std::string &stickRoot, bool requireAudio, std::ostream &log)
{
    const std::string pioneer = pioneerOf(stickRoot);
    // The readers Seabass itself scans with; the OneLibrary one rolls a hot
    // journal back the way the app does (sqlite_pending_journal.hpp).
    auto deviceTracks = rb::KaitaiRekordboxReader(pioneer).readTracks();
    auto oneTracks = infrastructure::onelibrary::OneLibraryReader(pioneer).readTracks();
    log << "DeviceLibrary " << deviceTracks.size() << " tracks, OneLibrary " << oneTracks.size() << " rows\n";

    std::map<std::string, std::vector<const domain::Track *>> deviceByPath;
    for (const auto &t : deviceTracks) {
        if (!t.filePath.empty()) {
            deviceByPath[pathToUtf8(pathFromUtf8(t.filePath).lexically_normal())].push_back(&t);
        }
    }
    std::map<std::string, std::vector<const domain::Track *>> oneByPath;
    std::map<std::string, int> titleCount;
    for (const auto &t : oneTracks) {
        ++titleCount[lower(t.title)];
        if (!t.filePath.empty()) {
            oneByPath[pathToUtf8(pathFromUtf8(t.filePath).lexically_normal())].push_back(&t);
        }
    }

    DbCopy copy(pioneer);
    std::vector<Candidate> out;
    for (const auto &[path, rows] : oneByPath) {
        auto device = deviceByPath.find(path);
        if (rows.size() != 1 || device == deviceByPath.end() || device->second.size() != 1) {
            continue;
        }
        const domain::Track &one = *rows.front();
        const domain::Track &dl = *device->second.front();
        if (one.analysisFile.empty() || one.analysisFile != dl.analysisFile || one.title.empty()
            || one.artist.empty() || titleCount[lower(one.title)] != 1) {
            continue;
        }
        const double duration = one.durationSeconds > 0 ? one.durationSeconds : dl.durationSeconds;
        if (duration < 150.0) {
            continue;
        }
        std::error_code ec;
        if (requireAudio && !fs::is_regular_file(pathFromUtf8(path), ec)) {
            continue;
        }
        if (!fs::is_regular_file(pathFromUtf8(rb::datAnlzPath(pioneer, one.analysisFile)), ec)) {
            continue;
        }
        if (!readAnalysisLists(pioneer, one.analysisFile).problems.empty()) {
            continue;
        }
        // The row's own content.path, as the writers will look it up.
        std::string contentPath =
            "/" + pathToGenericUtf8(pathFromUtf8(path).lexically_relative(pathFromUtf8(stickRoot).lexically_normal()));
        const auto ids = contentIdsAt(copy.db(), contentPath);
        if (ids.size() != 1 || std::to_string(ids.front()) != one.sourceId) {
            continue;
        }
        Candidate c;
        c.title = one.title;
        c.artist = one.artist;
        c.filePath = path;
        c.contentPath = contentPath;
        c.contentId = one.sourceId;
        c.deviceLibraryId = dl.sourceId;
        c.analysisFile = one.analysisFile;
        c.durationSeconds = duration;
        out.push_back(std::move(c));
    }
    log << out.size() << " candidate track(s) in both libraries with one shared analysis file\n";
    return out;
}

std::vector<Candidate> pickFive(const std::vector<Candidate> &candidates)
{
    const auto plain = [](const std::string &title) {
        return std::all_of(title.begin(), title.end(), [](unsigned char c) {
            return std::isalnum(c) || c == ' ' || c == '\'' || c == '-';
        });
    };
    std::vector<Candidate> sorted = candidates;
    std::stable_sort(sorted.begin(), sorted.end(), [&](const Candidate &a, const Candidate &b) {
        // Plain ASCII titles of three letters or more first, then shortest.
        const bool aGood = plain(a.title) && a.title.size() >= 3;
        const bool bGood = plain(b.title) && b.title.size() >= 3;
        if (aGood != bGood) {
            return aGood;
        }
        if (a.title.size() != b.title.size()) {
            return a.title.size() < b.title.size();
        }
        return lower(a.title) < lower(b.title);
    });
    std::vector<Candidate> five;
    std::set<char> initials;
    // Distinct first letters where the stick allows, so no two sit next to
    // each other in the deck's title list; then anything left.
    for (int pass = 0; pass < 2 && five.size() < 5; ++pass) {
        for (const auto &c : sorted) {
            if (five.size() == 5) {
                break;
            }
            const char initial = static_cast<char>(std::tolower(static_cast<unsigned char>(c.title.front())));
            const bool taken = std::any_of(five.begin(), five.end(),
                                           [&](const Candidate &f) { return f.filePath == c.filePath; });
            if (taken || (pass == 0 && initials.count(initial))) {
                continue;
            }
            initials.insert(initial);
            five.push_back(c);
        }
    }
    return five;
}

Expected expectedFor(int t)
{
    const auto entry = [](uint32_t slot, uint32_t ms) { return ListEntry{slot, ms, false, 0}; };
    const auto row = [](int64_t slot, int64_t ms, const std::string &comment) {
        return TableCue{slot, ms * 1000, ms * 1000, 0, comment};
    };
    switch (t) {
    case 1:
        return {{}, {}, {}, {row(1, 45000, "T1 TABLE")}};
    case 2:
        return {{entry(1, 20000), entry(2, 60000), entry(3, 100000)}, {}, {entry(1, 20000)}, {}};
    case 3: {
        const ListEntry loop{1, 30000, true, 38000};
        return {{loop}, {}, {loop}, {}};
    }
    case 4:
        return {{entry(2, 75000)}, {}, {entry(2, 75000)}, {row(2, 75000, "T4 SEABASS")}};
    case 5:
        return {{entry(1, 55000)}, {}, {entry(1, 55000)}, {row(1, 50000, "T5 TABLE")}};
    }
    throw std::logic_error("no such plant");
}

void plant(const std::string &stickRoot, const std::vector<Candidate> &five, std::ostream &log)
{
    if (five.size() != 5) {
        throw std::runtime_error("need five tracks, have " + std::to_string(five.size()));
    }
    const std::string pioneer = pioneerOf(stickRoot);

    // 1. The cue table, through the OneLibrary writer every Seabass save
    //    uses. It writes the analysis file first; step 3 reshapes that.
    {
        OneLibraryCueWriter writer(pioneer);
        writer.writeCuesForPath(five[0].filePath, {hot(1, 45000, "T1 TABLE")});
        writer.writeCuesForPath(five[1].filePath, {});
        writer.writeCuesForPath(five[2].filePath, {});
        writer.writeCuesForPath(five[3].filePath, {});
        writer.writeCuesForPath(five[4].filePath, {hot(1, 50000, "T5 TABLE")});
        writer.finishWriting();
        log << "table: T1 pad A 0:45, T5 pad A 0:50; T2, T3, T4 cleared (file and table)\n";
    }

    // 2. T4 through Add Cue for a OneLibrary row, the way the app's edit
    //    mode saves it: a staged AddCueChange run by the save loop, with
    //    the save's own backup and the shared OneLibrary writer.
    {
        gui::LibraryCatalogCache::instance().invalidateEveryCatalogOn(stickRoot);
        application::CancellationToken token;
        const QString root = gui::pathToQString(pathFromUtf8(pioneer));
        gui::SaveContext ctx(token, application::NullProgressReporter::instance(), {}, root, {});
        auto change = std::make_shared<gui::AddCueChange>(
            QStringLiteral("onelibrary"), root, QString::fromStdString(five[3].contentId), 75000.0,
            QStringLiteral("hot"), 2, QString(), QStringLiteral("T4 SEABASS"), false, 0.0,
            QString::fromStdString(five[3].title));
        const auto result = gui::runSaveLoop({change}, ctx);
        if (!result.error.isEmpty() || !result.appliedIds.contains(change->id())
            || result.skippedIds.contains(change->id())) {
            throw std::runtime_error("T4: Add Cue's save did not apply: " + result.error.toStdString());
        }
        // What the app's edit session does after a save that finished
        // (LibraryEditSession): the stick's note of a save in progress
        // goes, or the next session opened on it offers to undo an
        // "interrupted" save.
        infrastructure::backup::clearSaveInProgress(infrastructure::backup::backupDirForStickRoot(stickRoot));
        gui::LibraryCatalogCache::instance().invalidateEveryCatalogOn(stickRoot);
        log << "T4: Add Cue saved pad B 1:15 to the OneLibrary row (content_id " << five[3].contentId << ")\n";
    }

    // 3. The analysis files of T1, T2, T3 and T5, through the writer
    //    DeviceLibrary's cues go through (PCO2 and both PCOB lists in step).
    {
        rb::RekordboxCueWriter writer(pioneer);
        using Rewrite = rb::RekordboxCueWriter::Rewrite;
        writer.writeCuesToAnalysisFile(five[0].analysisFile, {}, Rewrite::Always);
        writer.writeCuesToAnalysisFile(five[1].analysisFile, {hot(1, 20000), hot(2, 60000), hot(3, 100000)},
                                       Rewrite::Always);
        writer.writeCuesToAnalysisFile(five[2].analysisFile, {hotLoop(1, 30000, 38000)}, Rewrite::Always);
        writer.writeCuesToAnalysisFile(five[4].analysisFile, {hot(1, 55000)}, Rewrite::Always);
        log << "files: T1 cleared, T2 A 0:20 B 1:00 C 1:40, T3 loop A 0:30-0:38, T5 pad A 0:55\n";
    }

    // 4. T2's legacy list down to pad A alone.
    trimLegacyHotListToPadA(pioneer, five[1].analysisFile);
    log << "T2: legacy (PCOB) hot list trimmed to pad A 0:20\n";
}

std::vector<std::string> verifyPlants(const std::string &stickRoot, const std::vector<Candidate> &five)
{
    std::vector<std::string> problems;
    const std::string pioneer = pioneerOf(stickRoot);
    const fs::path db = pathFromUtf8(OneLibraryCueWriter::dbPathFor(pioneer));
    if (infrastructure::hasPendingJournal(db)) {
        problems.push_back("exportLibrary.db has a hot journal beside it");
    }
    {
        fs::path wal = db;
        wal += "-wal";
        std::error_code ec;
        if (fs::exists(wal, ec) && fs::file_size(wal, ec) > 0) {
            problems.push_back("exportLibrary.db-wal is not empty: the library is not one file");
        }
    }
    DbCopy copy(pioneer);
    for (size_t i = 0; i < five.size(); ++i) {
        const int t = static_cast<int>(i) + 1;
        const std::string name = "T" + std::to_string(t) + " (" + five[i].title + ")";
        const Expected want = expectedFor(t);
        const AnalysisLists have = readAnalysisLists(pioneer, five[i].analysisFile);
        for (const auto &p : have.problems) {
            problems.push_back(name + ": " + p);
        }
        if (!have.datPresent) {
            problems.push_back(name + ": no .DAT");
        }
        const auto compare = [&](const char *list, const std::vector<ListEntry> &wanted, const std::vector<ListEntry> &got) {
            if (sortedBySlotAndTime(wanted) != sortedBySlotAndTime(got)) {
                problems.push_back(name + ": " + list + " is " + describe(got) + ", planted to be " + describe(wanted));
            }
        };
        compare("PCO2 hot", want.pco2Hot, have.pco2Hot);
        compare("PCO2 memory", {}, have.pco2Memory);
        compare(".EXT PCOB hot", want.extPcobHot, have.extPcobHot);
        compare(".EXT PCOB memory", {}, have.extPcobMemory);
        compare(".DAT PCOB hot", want.datPcobHot, have.datPcobHot);
        compare(".DAT PCOB memory", {}, have.datPcobMemory);

        const auto ids = contentIdsAt(copy.db(), five[i].contentPath);
        if (ids.size() != 1 || std::to_string(ids.front()) != five[i].contentId) {
            problems.push_back(name + ": OneLibrary no longer has exactly its one row at " + five[i].contentPath);
            continue;
        }
        const auto rows = cueRowsOf(copy.db(), ids.front());
        if (rows != want.table) {
            problems.push_back(name + ": cue table holds " + describe(rows) + ", planted to hold " + describe(want.table));
        }
    }
    return problems;
}

std::string card(const std::vector<Candidate> &five, const std::string &stickLabel)
{
    std::ostringstream out;
    out << "CARD  issue #59, OMNIS-DUO, stick " << stickLabel << "\n"
        << "Load each track from the stick's OneLibrary (Browse > Track, or search the title). No playlist:\n"
        << "Seabass cannot create a OneLibrary playlist. Look at the hot cue pads before touching anything;\n"
        << "do not store or delete cues, so the readback shows only what the deck does by itself.\n";
    const auto head = [&](int t) {
        const Candidate &c = five[static_cast<size_t>(t - 1)];
        const int seconds = static_cast<int>(c.durationSeconds);
        out << "\nT" << t << "  \"" << c.title << "\"  by " << c.artist << "  (" << seconds / 60 << ':'
            << std::setw(2) << std::setfill('0') << seconds % 60 << std::setfill(' ') << ")\n";
    };
    head(1);
    out << "    planted: pad A 0:45 in the cue table only; the analysis file holds no cue\n"
        << "    pad A at 0:45            = the OMNIS reads the cue table\n"
        << "    no pads lit              = it ignores the table\n";
    head(2);
    out << "    planted: legacy list (PCOB) pad A 0:20 only; modern list (PCO2) A 0:20, B 1:00, C 1:40\n"
        << "    only pad A (0:20)        = it reads the legacy PCOB list\n"
        << "    pads A, B and C          = it reads the modern PCO2 list\n";
    head(3);
    out << "    planted: hot loop on pad A 0:30 to 0:38, analysis file only (both lists), no table row\n"
        << "    pad A lit as a loop      = it shows loops held only in the analysis file\n"
        << "    pad A as a plain cue     = it reads the loop's start but not the loop\n"
        << "    no pad                   = it does not show it\n";
    head(4);
    out << "    planted: pad B 1:15 by Seabass's own Add Cue for the OneLibrary row (current master)\n"
        << "    pad B at 1:15            = what Seabass writes today reaches the OMNIS\n"
        << "    no pad B                 = it does not\n";
    head(5);
    out << "    planted: pad A 0:50 in the cue table, pad A 0:55 in the analysis file (both lists)\n"
        << "    pad A at 0:55            = the analysis file wins\n"
        << "    pad A at 0:50            = the table wins\n"
        << "\nAfter the session: onelibrary_cue_source_probe readback <stick> <snapshot.json>\n";
    return out.str();
}

QJsonObject snapshot(const std::string &stickRoot, const std::vector<Candidate> &five)
{
    const std::string pioneer = pioneerOf(stickRoot);
    QJsonObject snap;
    snap["tool"] = "onelibrary_cue_source_probe";
    snap["issue"] = 59;
    snap["stickRoot"] = QString::fromStdString(stickRoot);
    snap["takenAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    DbCopy copy(pioneer);
    QJsonArray tracks;
    for (size_t i = 0; i < five.size(); ++i) {
        const Candidate &c = five[i];
        QJsonObject t;
        t["t"] = static_cast<int>(i) + 1;
        t["title"] = QString::fromStdString(c.title);
        t["artist"] = QString::fromStdString(c.artist);
        t["filePath"] = QString::fromStdString(c.filePath);
        t["contentPath"] = QString::fromStdString(c.contentPath);
        t["contentId"] = QString::fromStdString(c.contentId);
        t["deviceLibraryId"] = QString::fromStdString(c.deviceLibraryId);
        t["analysisFile"] = QString::fromStdString(c.analysisFile);
        t["durationSeconds"] = c.durationSeconds;
        t["lists"] = toJson(readAnalysisLists(pioneer, c.analysisFile));
        QJsonArray rows;
        for (int64_t id : contentIdsAt(copy.db(), c.contentPath)) {
            QJsonObject row;
            row["contentId"] = QString::number(id);
            row["cues"] = toJson(cueRowsOf(copy.db(), id));
            row["content"] = toJson(rowsOf(copy.db(), "content", "content_id = ?", id));
            SqlCipherStatement count(copy.db(), "SELECT cueUpdateCount FROM content WHERE content_id = ?");
            count.bindInt64(1, id);
            row["cueUpdateCount"] = count.step() ? QString::fromStdString(count.columnText(0)) : QString();
            rows.append(row);
        }
        t["rows"] = rows;
        tracks.append(t);
    }
    snap["tracks"] = tracks;

    QJsonObject tables;
    QJsonObject history;
    for (const auto &name : allTables(copy.db())) {
        const auto rows = rowsOf(copy.db(), name);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (const auto &row : rows) {
            hash.addData(QByteArray::fromStdString(row + "\n"));
        }
        tables[QString::fromStdString(name)] =
            QJsonObject{{"rows", static_cast<int>(rows.size())}, {"digest", QString(hash.result().toHex())}};
        if (lower(name).find("history") != std::string::npos) {
            history[QString::fromStdString(name)] = toJson(rows);
        }
    }
    snap["tables"] = tables;
    snap["history"] = history;

    QJsonObject files;
    const fs::path root = pathFromUtf8(stickRoot);
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc)) {
            continue;
        }
        qint64 size = 0;
        const std::string digest = sha256Of(it->path(), size);
        files[QString::fromStdString(pathToGenericUtf8(it->path().lexically_relative(root)))] =
            QJsonObject{{"size", double(size)}, {"sha256", QString::fromStdString(digest)}};
    }
    snap["files"] = files;
    return snap;
}

namespace
{

// An empty value is a NULL column: the snapshot keeps columnText's reading.
std::string cueUpdateCountText(const QJsonObject &row)
{
    const std::string text = row["cueUpdateCount"].toString().toStdString();
    return text.empty() ? std::string("NULL") : text;
}

}  // namespace

int readback(const std::string &stickRoot, const QJsonObject &before, std::ostream &out)
{
    std::vector<Candidate> five;
    for (const auto &value : before["tracks"].toArray()) {
        const QJsonObject t = value.toObject();
        Candidate c;
        c.title = t["title"].toString().toStdString();
        c.artist = t["artist"].toString().toStdString();
        c.filePath = t["filePath"].toString().toStdString();
        c.contentPath = t["contentPath"].toString().toStdString();
        c.contentId = t["contentId"].toString().toStdString();
        c.deviceLibraryId = t["deviceLibraryId"].toString().toStdString();
        c.analysisFile = t["analysisFile"].toString().toStdString();
        c.durationSeconds = t["durationSeconds"].toDouble();
        five.push_back(c);
    }
    const QJsonObject now = snapshot(stickRoot, five);
    const std::string pioneer = pioneerOf(stickRoot);
    int differences = 0;

    out << "readback of " << stickRoot << " against the snapshot taken " << before["takenAt"].toString().toStdString()
        << "\n";

    // The five tracks.
    const QJsonArray beforeTracks = before["tracks"].toArray();
    const QJsonArray nowTracks = now["tracks"].toArray();
    std::set<std::string> plantedFiles;
    for (qsizetype i = 0; i < beforeTracks.size(); ++i) {
        const QJsonObject b = beforeTracks[i].toObject();
        const QJsonObject n = nowTracks[i].toObject();
        out << "\nT" << b["t"].toInt() << " \"" << b["title"].toString().toStdString() << "\" by "
            << b["artist"].toString().toStdString() << "  (" << b["analysisFile"].toString().toStdString() << ")\n";
        plantedFiles.insert(pathToGenericUtf8(pathFromUtf8(rb::extAnlzPath(pioneer, b["analysisFile"].toString().toStdString()))
                                           .lexically_relative(pathFromUtf8(stickRoot))));
        plantedFiles.insert(pathToGenericUtf8(pathFromUtf8(rb::datAnlzPath(pioneer, b["analysisFile"].toString().toStdString()))
                                           .lexically_relative(pathFromUtf8(stickRoot))));
        const QJsonObject bl = b["lists"].toObject();
        const QJsonObject nl = n["lists"].toObject();
        for (const auto &[name, member] : listNames()) {
            const auto was = listFromJson(bl[name].toArray());
            const auto is = listFromJson(nl[name].toArray());
            if (was != is) {
                ++differences;
                out << "  CHANGED " << name << ": was " << describe(was) << ", now " << describe(is) << "\n";
            } else {
                out << "  same    " << name << ": " << describe(is) << "\n";
            }
        }
        if (bl["problems"] != nl["problems"]) {
            ++differences;
            out << "  CHANGED analysis file problems: now";
            for (const auto &p : stringsFromJson(nl["problems"].toArray())) {
                out << " [" << p << "]";
            }
            out << "\n";
        }
        if (b["rows"] != n["rows"]) {
            ++differences;
            const auto rowsText = [](const QJsonArray &rows) {
                std::string text;
                for (const auto &value : rows) {
                    const QJsonObject r = value.toObject();
                    text += "content_id " + r["contentId"].toString().toStdString() + " cueUpdateCount "
                        + cueUpdateCountText(r) + " cues [";
                    std::string cues;
                    for (const auto &cue : r["cues"].toArray()) {
                        cues += (cues.empty() ? "" : ", ") + cue.toObject()["text"].toString().toStdString();
                    }
                    text += cues + "]; ";
                }
                return text.empty() ? std::string("(no row)") : text;
            };
            out << "  CHANGED OneLibrary row: was " << rowsText(b["rows"].toArray()) << "\n"
                << "                          now " << rowsText(n["rows"].toArray()) << "\n";
            const QJsonArray br = b["rows"].toArray();
            const QJsonArray nr = n["rows"].toArray();
            if (br.size() == 1 && nr.size() == 1 && br[0].toObject()["content"] != nr[0].toObject()["content"]) {
                out << "    content row was: " << br[0].toObject()["content"].toArray()[0].toString().toStdString() << "\n"
                    << "    content row now: " << nr[0].toObject()["content"].toArray()[0].toString().toStdString() << "\n";
            }
        } else {
            const QJsonArray nr = n["rows"].toArray();
            for (const auto &value : nr) {
                const QJsonObject r = value.toObject();
                std::string cues;
                for (const auto &cue : r["cues"].toArray()) {
                    cues += (cues.empty() ? "" : ", ") + cue.toObject()["text"].toString().toStdString();
                }
                out << "  same    cue table: " << (cues.empty() ? "(no rows)" : cues) << "; content row unchanged"
                    << " (cueUpdateCount " << cueUpdateCountText(r) << ")\n";
            }
        }
    }

    // exportLibrary.db, table by table.
    out << "\nexportLibrary.db tables:\n";
    const QJsonObject bt = before["tables"].toObject();
    const QJsonObject nt = now["tables"].toObject();
    bool anyTable = false;
    QStringList names = bt.keys();
    for (const auto &name : nt.keys()) {
        if (!names.contains(name)) {
            names.append(name);
        }
    }
    for (const auto &name : names) {
        if (bt[name] == nt[name]) {
            continue;
        }
        anyTable = true;
        ++differences;
        out << "  CHANGED " << name.toStdString() << ": rows " << bt[name].toObject()["rows"].toInt() << " -> "
            << nt[name].toObject()["rows"].toInt() << "\n";
    }
    if (!anyTable) {
        out << "  every table as it was\n";
    }
    const QJsonObject bh = before["history"].toObject();
    const QJsonObject nh = now["history"].toObject();
    for (const auto &name : nh.keys()) {
        const auto was = stringsFromJson(bh[name].toArray());
        const auto is = stringsFromJson(nh[name].toArray());
        const std::set<std::string> wasSet(was.begin(), was.end());
        const std::set<std::string> isSet(is.begin(), is.end());
        for (const auto &row : is) {
            if (!wasSet.count(row)) {
                out << "  + " << name.toStdString() << ": " << row << "\n";
            }
        }
        for (const auto &row : was) {
            if (!isSet.count(row)) {
                out << "  - " << name.toStdString() << ": " << row << "\n";
            }
        }
    }

    // Every file on the stick.
    out << "\nfiles:\n";
    const QJsonObject bf = before["files"].toObject();
    const QJsonObject nf = now["files"].toObject();
    bool anyFile = false;
    const auto reportAnlz = [&](const std::string &rel) {
        // Another track's analysis file the deck wrote: say what it holds.
        const std::string upper = [&] {
            std::string u = rel;
            for (char &c : u) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return u;
        }();
        if (upper.find("USBANLZ") == std::string::npos || plantedFiles.count(rel)) {
            return;
        }
        if (upper.size() < 4 || (upper.substr(upper.size() - 4) != ".EXT" && upper.substr(upper.size() - 4) != ".DAT")) {
            return;
        }
        const std::string analyze = "/" + rel.substr(0, rel.size() - 4) + ".DAT";
        const AnalysisLists lists = readAnalysisLists(pioneer, analyze);
        for (const auto &[name, member] : listNames()) {
            out << "      " << name << ": " << describe(lists.*member) << "\n";
        }
    };
    for (const auto &name : bf.keys()) {
        if (!nf.contains(name)) {
            anyFile = true;
            ++differences;
            out << "  REMOVED " << name.toStdString() << "\n";
        } else if (bf[name] != nf[name]) {
            anyFile = true;
            ++differences;
            out << "  CHANGED " << name.toStdString() << " (" << bf[name].toObject()["size"].toDouble() << " -> "
                << nf[name].toObject()["size"].toDouble() << " bytes)\n";
            reportAnlz(name.toStdString());
        }
    }
    for (const auto &name : nf.keys()) {
        if (!bf.contains(name)) {
            anyFile = true;
            ++differences;
            out << "  ADDED   " << name.toStdString() << " (" << nf[name].toObject()["size"].toDouble() << " bytes)\n";
            reportAnlz(name.toStdString());
        }
    }
    if (!anyFile) {
        out << "  every file as it was\n";
    }

    // This readback must not have changed anything itself.
    const QJsonObject after = snapshot(stickRoot, five);
    if (after["files"] != now["files"]) {
        ++differences;
        out << "\nWARNING: the stick changed while this readback ran\n";
    }
    out << "\n" << differences << " difference(s) from the prepared state\n";
    return differences;
}

}  // namespace seabass::omnis59
