// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// engine_import_probe's record format, its planted-case list, and the
// comparison of two records. Kept apart from the tool itself, and free of
// any reader or writer, so engine_import_probe_compare_test can feed it
// records built by hand.
//
// A record is a set of (section, entity, field) -> value. Sections:
//
//   M   meta: the pdb sequence, Engine's import counter, what the player
//       would do on insert
//   EI  Engine's Information row, one field per column
//   ET  one Engine Track row, keyed by the audio file's stick-relative
//       path (not its id: an import that rebuilds the rows gives them new
//       ids, and the path is what stays)
//   EP  one Engine playlist, keyed by its full path ("Folder/Name")
//   EA  the Engine library's AlbumArt table, summarised
//   EF  one file under "Engine Library", keyed by its relative path
//   RT  one rekordbox track, keyed the same way as ET
//   RP  one rekordbox playlist, keyed the same way as EP
//   RO  one OneLibrary (exportLibrary.db) track, keyed the same way as ET
//
// ET and RT share their field names wherever the two catalogs hold the
// same thing (title, key, bpm, rating, comment, cue.hot.N, cue.loop.N,
// cue.main, playlists), so a case can ask "does Engine now say what
// rekordbox said?" by looking the same field up in the other section.
//
// A value that is a set (a playlist's entries, a track's playlists)
// holds its elements separated by a tab; the file format escapes tabs, so
// a record line stays one line.

#pragma once

#include <algorithm>
#include <cstddef>
#include <istream>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace seabass::probe
{

constexpr const char *RecordHeader = "# engine import probe record v1";
constexpr const char *CasesHeader = "# engine import probe cases v1";
constexpr char SetSeparator = '\t';

using RecordKey = std::tuple<std::string, std::string, std::string>;

struct Record
{
    std::map<RecordKey, std::string> values;

    void set(const std::string &section, const std::string &entity, const std::string &field, std::string value)
    {
        values[{section, entity, field}] = std::move(value);
    }

    std::optional<std::string> get(const std::string &section, const std::string &entity,
                                   const std::string &field) const
    {
        const auto it = values.find({section, entity, field});
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // Every entity a section holds.
    std::set<std::string> entities(const std::string &section) const
    {
        std::set<std::string> out;
        for (const auto &[key, value] : values) {
            if (std::get<0>(key) == section) {
                out.insert(std::get<1>(key));
            }
        }
        return out;
    }

    // Every (field, value) of one entity.
    std::map<std::string, std::string> fields(const std::string &section, const std::string &entity) const
    {
        std::map<std::string, std::string> out;
        const auto first = values.lower_bound({section, entity, std::string()});
        for (auto it = first; it != values.end(); ++it) {
            if (std::get<0>(it->first) != section || std::get<1>(it->first) != entity) {
                break;
            }
            out[std::get<2>(it->first)] = it->second;
        }
        return out;
    }
};

inline std::string escapeField(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out += c;
        }
    }
    return out;
}

inline std::string unescapeField(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\' || i + 1 == in.size()) {
            out += in[i];
            continue;
        }
        const char next = in[++i];
        out += next == 't' ? '\t' : next == 'n' ? '\n' : next == 'r' ? '\r' : next;
    }
    return out;
}

// Splits one line on raw tabs (escaped ones are inside fields).
inline std::vector<std::string> splitLine(const std::string &line)
{
    std::vector<std::string> parts;
    std::string current;
    for (const char c : line) {
        if (c == '\t') {
            parts.push_back(unescapeField(current));
            current.clear();
        } else {
            current += c;
        }
    }
    parts.push_back(unescapeField(current));
    return parts;
}

inline std::string joinSet(const std::vector<std::string> &elements)
{
    std::string out;
    for (const auto &e : elements) {
        if (!out.empty()) {
            out += SetSeparator;
        }
        out += e;
    }
    return out;
}

inline std::set<std::string> splitSet(const std::string &value)
{
    std::set<std::string> out;
    if (value.empty()) {
        return out;
    }
    std::string current;
    for (const char c : value) {
        if (c == SetSeparator) {
            out.insert(current);
            current.clear();
        } else {
            current += c;
        }
    }
    out.insert(current);
    return out;
}

inline void writeRecord(std::ostream &out, const Record &record)
{
    out << RecordHeader << "\n";
    for (const auto &[key, value] : record.values) {
        out << escapeField(std::get<0>(key)) << '\t' << escapeField(std::get<1>(key)) << '\t'
            << escapeField(std::get<2>(key)) << '\t' << escapeField(value) << '\n';
    }
}

inline bool readRecord(std::istream &in, Record &record, std::string *error)
{
    std::string line;
    if (!std::getline(in, line) || line != RecordHeader) {
        if (error) {
            *error = "not an engine import probe record (first line is not \"" + std::string(RecordHeader) + "\")";
        }
        return false;
    }
    int lineNumber = 1;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto parts = splitLine(line);
        if (parts.size() != 4) {
            if (error) {
                *error = "line " + std::to_string(lineNumber) + " has " + std::to_string(parts.size())
                    + " fields, not 4";
            }
            return false;
        }
        record.set(parts[0], parts[1], parts[2], parts[3]);
    }
    return true;
}

// One value a case watches. `mode` is "value" (compared as a string),
// "set" (compared as a set; this is where MERGED can come from) or
// "member:<element>" (whether that element is in the set: present or
// absent, so a removal reads DELETED and an insertion ADDED).
struct Item
{
    std::string section;
    std::string entity;
    std::string field;
    std::string mode = "value";

    bool isSet() const { return mode == "set"; }
    std::optional<std::string> member() const
    {
        if (mode.rfind("member:", 0) == 0) {
            return mode.substr(7);
        }
        return std::nullopt;
    }
    // The same field on the rekordbox side: ET -> RT, EP -> RP.
    std::string rekordboxSection() const
    {
        if (section == "ET") {
            return "RT";
        }
        if (section == "EP") {
            return "RP";
        }
        return {};
    }
};

// One planted difference. `status` is "planted" (this tool wrote it),
// "existing" (the stick already had it and nothing was written),
// "observed" (watched, not changed: a control), or "not planted" (with
// the reason in `how`).
struct Case
{
    std::string id;
    std::string status;
    std::string title;
    std::string track;      // stick-relative path, or a playlist name
    std::string engine;     // what Engine holds after planting
    std::string rekordbox;  // what rekordbox holds after planting
    std::string how;        // how it was written, or why it was not
    std::vector<Item> items;  // the first one is the case's verdict
};

inline void writeCases(std::ostream &out, const std::vector<Case> &cases)
{
    out << CasesHeader << "\n";
    out << "# case\tid\tstatus\ttitle\ttrack\tengine\trekordbox\thow\n";
    out << "# watch\tid\tsection\tentity\tfield\tmode\n";
    for (const Case &c : cases) {
        out << "case\t" << escapeField(c.id) << '\t' << escapeField(c.status) << '\t' << escapeField(c.title) << '\t'
            << escapeField(c.track) << '\t' << escapeField(c.engine) << '\t' << escapeField(c.rekordbox) << '\t'
            << escapeField(c.how) << '\n';
        for (const Item &item : c.items) {
            out << "watch\t" << escapeField(c.id) << '\t' << escapeField(item.section) << '\t'
                << escapeField(item.entity) << '\t' << escapeField(item.field) << '\t' << escapeField(item.mode)
                << '\n';
        }
    }
}

inline bool readCases(std::istream &in, std::vector<Case> &cases, std::string *error)
{
    std::string line;
    if (!std::getline(in, line) || line != CasesHeader) {
        if (error) {
            *error = "not an engine import probe case list (first line is not \"" + std::string(CasesHeader) + "\")";
        }
        return false;
    }
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto parts = splitLine(line);
        if (parts[0] == "case" && parts.size() == 8) {
            cases.push_back(Case{parts[1], parts[2], parts[3], parts[4], parts[5], parts[6], parts[7], {}});
        } else if (parts[0] == "watch" && parts.size() == 6) {
            auto it = std::find_if(cases.begin(), cases.end(), [&](const Case &c) { return c.id == parts[1]; });
            if (it == cases.end()) {
                if (error) {
                    *error = "a watch line names case \"" + parts[1] + "\", which no case line before it declares";
                }
                return false;
            }
            it->items.push_back(Item{parts[2], parts[3], parts[4], parts[5]});
        } else {
            if (error) {
                *error = "unreadable line: " + line;
            }
            return false;
        }
    }
    return true;
}

enum class Verdict { Kept, Overwritten, Deleted, Added, Merged };

inline const char *verdictName(Verdict v)
{
    switch (v) {
    case Verdict::Kept:
        return "KEPT";
    case Verdict::Overwritten:
        return "OVERWRITTEN";
    case Verdict::Deleted:
        return "DELETED";
    case Verdict::Added:
        return "ADDED";
    case Verdict::Merged:
        return "MERGED";
    }
    return "?";
}

// What happened to one Engine value across the import. `rekordbox` is
// what the rekordbox side held for the same thing before it, which is
// only needed to tell a merge (Engine now holds both sides) from an
// overwrite (Engine now holds rekordbox's).
inline Verdict classify(const std::optional<std::string> &before, const std::optional<std::string> &after,
                        const std::optional<std::string> &rekordbox, bool isSet)
{
    if (before == after) {
        return Verdict::Kept;
    }
    if (!before) {
        return Verdict::Added;
    }
    if (!after) {
        return Verdict::Deleted;
    }
    if (isSet) {
        const std::set<std::string> b = splitSet(*before);
        const std::set<std::string> a = splitSet(*after);
        const std::set<std::string> r = rekordbox ? splitSet(*rekordbox) : std::set<std::string>();
        if (a == r) {
            return Verdict::Overwritten;
        }
        std::set<std::string> both = b;
        both.insert(r.begin(), r.end());
        if (a == both) {
            return Verdict::Merged;
        }
    }
    return Verdict::Overwritten;
}

struct ItemOutcome
{
    Item item;
    std::optional<std::string> before;
    std::optional<std::string> after;
    std::optional<std::string> rekordbox;
    // The same field in exportLibrary.db, for ET items, when recorded.
    std::optional<std::string> oneLibrary;
    Verdict verdict = Verdict::Kept;
};

struct CaseOutcome
{
    Case planted;
    std::vector<ItemOutcome> items;
    // Fields of the case's own track that changed and that no item
    // watches: "field: old -> new".
    std::vector<std::string> alsoChanged;
    bool counted() const { return planted.status != "not planted" && !items.empty(); }
    Verdict verdict() const { return items.empty() ? Verdict::Kept : items.front().verdict; }
};

struct CompareResult
{
    std::vector<CaseOutcome> cases;
    std::vector<std::string> outside;  // lines, already worded
    std::map<Verdict, int> counts;
    int notPlanted = 0;
};

inline std::string shown(const std::optional<std::string> &value)
{
    if (!value) {
        return "(none)";
    }
    if (value->empty()) {
        return "\"\"";
    }
    std::string out = *value;
    for (char &c : out) {
        if (c == SetSeparator) {
            c = ',';
        }
    }
    if (out.size() > 160) {
        out = out.substr(0, 157) + "...";
    }
    return out;
}

inline std::optional<std::string> itemValue(const Record &record, const Item &item, const std::string &section)
{
    const auto value = record.get(section, item.entity, item.field);
    if (const auto element = item.member()) {
        if (value && splitSet(*value).count(*element) > 0) {
            return std::string("member");
        }
        return std::nullopt;
    }
    return value;
}

namespace detail
{

struct FieldChange
{
    int count = 0;
    std::vector<std::string> examples;
};

inline void noteChange(std::map<std::string, FieldChange> &changes, const std::string &field, const std::string &entity,
                       const std::optional<std::string> &before, const std::optional<std::string> &after)
{
    FieldChange &change = changes[field];
    ++change.count;
    if (change.examples.size() < 2) {
        change.examples.push_back(entity + ": " + shown(before) + " -> " + shown(after));
    }
}

inline void reportFieldChanges(std::vector<std::string> &out, const std::map<std::string, FieldChange> &changes,
                               const std::string &noun)
{
    std::vector<std::pair<std::string, FieldChange>> sorted(changes.begin(), changes.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const auto &a, const auto &b) { return a.second.count > b.second.count; });
    for (const auto &[field, change] : sorted) {
        out.push_back("    " + field + ": changed on " + std::to_string(change.count) + " " + noun);
        for (const auto &example : change.examples) {
            out.push_back("      e.g. " + example);
        }
    }
}

inline int countWhere(const Record &record, const std::string &section, const std::string &field,
                      bool (*predicate)(const std::string &))
{
    int n = 0;
    for (const auto &entity : record.entities(section)) {
        const auto value = record.get(section, entity, field);
        if (value && predicate(*value)) {
            ++n;
        }
    }
    return n;
}

inline bool isImportedArt(const std::string &v)
{
    return v.rfind("imported:", 0) == 0;
}
inline bool isOne(const std::string &v)
{
    return v == "1";
}
inline bool isNull(const std::string &v)
{
    return v == "NULL";
}

}  // namespace detail

inline CompareResult compareRecords(const Record &before, const Record &after, const std::vector<Case> &cases)
{
    CompareResult result;
    std::set<std::string> plantedTracks;
    std::set<std::string> plantedPlaylists;

    for (const Case &c : cases) {
        CaseOutcome outcome;
        outcome.planted = c;
        std::set<std::string> watchedFields;
        for (const Item &item : c.items) {
            if (item.section == "ET") {
                plantedTracks.insert(item.entity);
                watchedFields.insert(item.field);
            } else if (item.section == "EP") {
                plantedPlaylists.insert(item.entity);
            }
            ItemOutcome io;
            io.item = item;
            io.before = itemValue(before, item, item.section);
            io.after = itemValue(after, item, item.section);
            if (!item.rekordboxSection().empty()) {
                io.rekordbox = itemValue(before, item, item.rekordboxSection());
            }
            if (item.section == "ET") {
                io.oneLibrary = itemValue(before, item, "RO");
            }
            io.verdict = classify(io.before, io.after, io.rekordbox, item.isSet());
            outcome.items.push_back(std::move(io));
        }
        if (!c.items.empty() && c.items.front().section == "ET") {
            const std::string &path = c.items.front().entity;
            const auto b = before.fields("ET", path);
            const auto a = after.fields("ET", path);
            std::set<std::string> names;
            for (const auto &[k, v] : b) {
                names.insert(k);
            }
            for (const auto &[k, v] : a) {
                names.insert(k);
            }
            for (const auto &name : names) {
                if (watchedFields.count(name) > 0) {
                    continue;
                }
                const auto bi = b.find(name);
                const auto ai = a.find(name);
                const std::optional<std::string> bv = bi == b.end() ? std::nullopt : std::optional(bi->second);
                const std::optional<std::string> av = ai == a.end() ? std::nullopt : std::optional(ai->second);
                if (bv != av) {
                    outcome.alsoChanged.push_back(name + ": " + shown(bv) + " -> " + shown(av));
                }
            }
        }
        if (outcome.counted()) {
            result.counts[outcome.verdict()]++;
        } else {
            result.notPlanted++;
        }
        result.cases.push_back(std::move(outcome));
    }

    std::vector<std::string> &out = result.outside;

    // The import's own bookkeeping.
    out.push_back("Import bookkeeping:");
    for (const char *field : {"pdb.sequence", "engine.counter", "prompt"}) {
        const auto b = before.get("M", "-", field);
        const auto a = after.get("M", "-", field);
        out.push_back(std::string("  ") + field + ": " + shown(b) + (b == a ? " (unchanged)" : " -> " + shown(a)));
    }
    {
        const auto b = before.fields("EI", "-");
        const auto a = after.fields("EI", "-");
        std::set<std::string> names;
        for (const auto &[k, v] : b) {
            names.insert(k);
        }
        for (const auto &[k, v] : a) {
            names.insert(k);
        }
        int changed = 0;
        for (const auto &name : names) {
            const auto bi = b.find(name);
            const auto ai = a.find(name);
            const std::optional<std::string> bv = bi == b.end() ? std::nullopt : std::optional(bi->second);
            const std::optional<std::string> av = ai == a.end() ? std::nullopt : std::optional(ai->second);
            if (bv != av) {
                out.push_back("  Information." + name + ": " + shown(bv) + " -> " + shown(av));
                ++changed;
            }
        }
        if (changed == 0) {
            out.push_back("  Information row: unchanged");
        }
    }

    // The player's clock, where it left a mark: SQLite triggers stamp
    // Track.lastEditTime with strftime('%s') on the player, which is Unix
    // seconds by the player's own clock; files it rewrote carry a
    // modification time (on FAT a local time the computer interprets).
    // Both set against when the after record was taken on this computer.
    {
        long long newestEdit = 0;
        std::string newestEditTrack;
        for (const auto &path : after.entities("ET")) {
            const auto a = after.get("ET", path, "col.lastEditTime");
            if (!a || a == before.get("ET", path, "col.lastEditTime")) {
                continue;
            }
            try {
                const long long v = std::stoll(*a);
                if (v > newestEdit) {
                    newestEdit = v;
                    newestEditTrack = path;
                }
            } catch (const std::exception &) {
            }
        }
        long long newestFile = 0;
        std::string newestFileName;
        for (const auto &f : after.entities("EF")) {
            const auto a = after.get("EF", f, "mtime.unix");
            if (!a || after.get("EF", f, "mtime") == before.get("EF", f, "mtime")) {
                continue;
            }
            try {
                const long long v = std::stoll(*a);
                if (v > newestFile) {
                    newestFile = v;
                    newestFileName = f;
                }
            } catch (const std::exception &) {
            }
        }
        const auto recorded = after.get("M", "-", "recordedAt.unix");
        long long recordedAt = 0;
        if (recorded) {
            try {
                recordedAt = std::stoll(*recorded);
            } catch (const std::exception &) {
            }
        }
        out.push_back("Clocks (Unix seconds; the gaps include the time between eject and the after record):");
        out.push_back("  after record taken on this computer: " + (recordedAt ? std::to_string(recordedAt) : std::string("not recorded")));
        if (newestEdit) {
            out.push_back("  newest Track.lastEditTime the import wrote (player clock): " + std::to_string(newestEdit) + " on "
                          + newestEditTrack + (recordedAt ? ", " + std::to_string(recordedAt - newestEdit) + " s before the record" : ""));
        } else {
            out.push_back("  no Track.lastEditTime changed, so no player clock reading from the database");
        }
        if (newestFile) {
            out.push_back("  newest rewritten file: " + newestFileName + " at " + std::to_string(newestFile)
                          + (recordedAt ? ", " + std::to_string(recordedAt - newestFile) + " s before the record" : ""));
        }
    }

    // Engine tracks outside the matrix.
    {
        const auto beforeTracks = before.entities("ET");
        const auto afterTracks = after.entities("ET");
        std::vector<std::string> added;
        std::vector<std::string> removed;
        std::map<std::string, detail::FieldChange> changes;
        std::set<std::string> touched;
        for (const auto &path : afterTracks) {
            if (beforeTracks.count(path) == 0) {
                added.push_back(path);
            }
        }
        for (const auto &path : beforeTracks) {
            if (afterTracks.count(path) == 0) {
                if (plantedTracks.count(path) == 0) {
                    removed.push_back(path);
                }
                continue;
            }
            if (plantedTracks.count(path) > 0) {
                continue;
            }
            const auto b = before.fields("ET", path);
            const auto a = after.fields("ET", path);
            std::set<std::string> names;
            for (const auto &[k, v] : b) {
                names.insert(k);
            }
            for (const auto &[k, v] : a) {
                names.insert(k);
            }
            for (const auto &name : names) {
                const auto bi = b.find(name);
                const auto ai = a.find(name);
                const std::optional<std::string> bv = bi == b.end() ? std::nullopt : std::optional(bi->second);
                const std::optional<std::string> av = ai == a.end() ? std::nullopt : std::optional(ai->second);
                if (bv != av) {
                    detail::noteChange(changes, name, path, bv, av);
                    touched.insert(path);
                }
            }
        }
        const auto isRow = [](const Record &r, const std::string &path) { return r.get("ET", path, "row").has_value(); };
        const std::size_t rowsBefore = std::count_if(beforeTracks.begin(), beforeTracks.end(),
                                                     [&](const std::string &p) { return isRow(before, p); });
        const std::size_t rowsAfter = std::count_if(afterTracks.begin(), afterTracks.end(),
                                                    [&](const std::string &p) { return isRow(after, p); });
        const std::size_t unplanted = rowsBefore
            - std::count_if(beforeTracks.begin(), beforeTracks.end(),
                            [&](const std::string &p) { return plantedTracks.count(p) > 0 && isRow(before, p); });
        out.push_back("Engine tracks outside the matrix: " + std::to_string(unplanted) + " before, "
                      + std::to_string(touched.size()) + " changed, " + std::to_string(removed.size())
                      + " removed, " + std::to_string(added.size()) + " added (" + std::to_string(rowsBefore)
                      + " rows before, " + std::to_string(rowsAfter) + " after)");
        for (std::size_t i = 0; i < removed.size() && i < 10; ++i) {
            out.push_back("    removed: " + removed[i]);
        }
        for (std::size_t i = 0; i < added.size() && i < 10; ++i) {
            out.push_back("    added: " + added[i]);
        }
        detail::reportFieldChanges(out, changes, "unplanted tracks");
    }

    // Library-wide counts that the talk is likely to quote.
    {
        const int artBefore = detail::countWhere(before, "ET", "art", detail::isImportedArt);
        const int artAfter = detail::countWhere(after, "ET", "art", detail::isImportedArt);
        out.push_back("Cover references of the form \"image://fileart//...\": " + std::to_string(artBefore) + " before, "
                      + std::to_string(artAfter) + " after");
        const int analysedBefore = detail::countWhere(before, "ET", "col.isAnalyzed", detail::isOne);
        const int analysedAfter = detail::countWhere(after, "ET", "col.isAnalyzed", detail::isOne);
        out.push_back("Tracks with isAnalyzed = 1: " + std::to_string(analysedBefore) + " before, "
                      + std::to_string(analysedAfter) + " after");
        const int gridBefore = detail::countWhere(before, "ET", "perf.beatData", detail::isNull);
        const int gridAfter = detail::countWhere(after, "ET", "perf.beatData", detail::isNull);
        out.push_back("Tracks with no beat data (PerformanceData.beatData NULL): " + std::to_string(gridBefore)
                      + " before, " + std::to_string(gridAfter) + " after");
        for (const char *field : {"rows", "rowsWithImage", "textHashes", "blobHashes", "importedReferences"}) {
            const auto b = before.get("EA", "-", field);
            const auto a = after.get("EA", "-", field);
            out.push_back(std::string("AlbumArt ") + field + ": " + shown(b) + (b == a ? " (unchanged)" : " -> " + shown(a)));
        }
    }

    // Playlists.
    {
        const auto b = before.entities("EP");
        const auto a = after.entities("EP");
        int changed = 0;
        out.push_back("Engine playlists: " + std::to_string(b.size()) + " before, " + std::to_string(a.size())
                      + " after");
        for (const auto &name : b) {
            if (a.count(name) == 0) {
                out.push_back("    removed: " + name);
                ++changed;
            }
        }
        for (const auto &name : a) {
            if (b.count(name) == 0) {
                out.push_back("    added: " + name + " (" + shown(after.get("EP", name, "count")) + " tracks)");
                ++changed;
            }
        }
        for (const auto &name : b) {
            if (a.count(name) == 0) {
                continue;
            }
            const auto be = before.get("EP", name, "entries");
            const auto ae = after.get("EP", name, "entries");
            const auto bid = before.get("EP", name, "id");
            const auto aid = after.get("EP", name, "id");
            if (be != ae || bid != aid) {
                const auto bs = splitSet(be.value_or(""));
                const auto as = splitSet(ae.value_or(""));
                int gained = 0;
                int lost = 0;
                for (const auto &e : as) {
                    gained += bs.count(e) == 0 ? 1 : 0;
                }
                for (const auto &e : bs) {
                    lost += as.count(e) == 0 ? 1 : 0;
                }
                std::string line = "    changed: " + name + (plantedPlaylists.count(name) ? " (planted)" : "") + ": "
                    + std::to_string(bs.size()) + " -> " + std::to_string(as.size()) + " tracks, +"
                    + std::to_string(gained) + " -" + std::to_string(lost);
                if (be == ae) {
                    line += ", same tracks in the same order";
                } else if (bs == as) {
                    line += ", same tracks, order changed";
                }
                if (bid != aid) {
                    line += ", row id " + shown(bid) + " -> " + shown(aid) + " (rebuilt)";
                }
                out.push_back(line);
                ++changed;
            }
        }
        if (changed == 0) {
            out.push_back("    none changed");
        }
    }

    // Files under Engine Library.
    {
        const auto b = before.entities("EF");
        const auto a = after.entities("EF");
        std::vector<std::string> added;
        std::vector<std::string> removed;
        std::vector<std::string> resized;
        std::vector<std::string> touchedOnly;
        for (const auto &f : a) {
            if (b.count(f) == 0) {
                added.push_back(f + " (" + shown(after.get("EF", f, "size")) + " bytes)");
            }
        }
        for (const auto &f : b) {
            if (a.count(f) == 0) {
                removed.push_back(f + " (" + shown(before.get("EF", f, "size")) + " bytes)");
                continue;
            }
            const auto bs = before.get("EF", f, "size");
            const auto as = after.get("EF", f, "size");
            if (bs != as) {
                resized.push_back(f + ": " + shown(bs) + " -> " + shown(as) + " bytes");
            } else if (before.get("EF", f, "mtime") != after.get("EF", f, "mtime")) {
                touchedOnly.push_back(f);
            }
        }
        out.push_back("Files under Engine Library: " + std::to_string(b.size()) + " before, " + std::to_string(a.size())
                      + " after; " + std::to_string(added.size()) + " added, " + std::to_string(removed.size())
                      + " removed, " + std::to_string(resized.size()) + " changed size, "
                      + std::to_string(touchedOnly.size()) + " rewritten at the same size");
        const auto list = [&out](const char *what, const std::vector<std::string> &files) {
            for (std::size_t i = 0; i < files.size() && i < 15; ++i) {
                out.push_back(std::string("    ") + what + ": " + files[i]);
            }
            if (files.size() > 15) {
                out.push_back(std::string("    ") + what + ": ... and " + std::to_string(files.size() - 15) + " more");
            }
        };
        list("added", added);
        list("removed", removed);
        list("size", resized);
        list("rewritten", touchedOnly);
    }

    // The rekordbox side, which the import is supposed to leave alone.
    {
        std::map<std::string, detail::FieldChange> changes;
        int rows = 0;
        for (const char *section : {"RT", "RP", "RO"}) {
            const auto b = before.entities(section);
            const auto a = after.entities(section);
            std::set<std::string> all(b.begin(), b.end());
            all.insert(a.begin(), a.end());
            for (const auto &entity : all) {
                const auto bf = before.fields(section, entity);
                const auto af = after.fields(section, entity);
                if (bf == af) {
                    continue;
                }
                ++rows;
                std::set<std::string> names;
                for (const auto &[k, v] : bf) {
                    names.insert(k);
                }
                for (const auto &[k, v] : af) {
                    names.insert(k);
                }
                for (const auto &name : names) {
                    const auto bi = bf.find(name);
                    const auto ai = af.find(name);
                    const std::optional<std::string> bv = bi == bf.end() ? std::nullopt : std::optional(bi->second);
                    const std::optional<std::string> av = ai == af.end() ? std::nullopt : std::optional(ai->second);
                    if (bv != av) {
                        detail::noteChange(changes, std::string(section) + " " + name, entity, bv, av);
                    }
                }
            }
        }
        if (rows == 0) {
            out.push_back("rekordbox side: unchanged (every track and playlist reads the same)");
        } else {
            out.push_back("rekordbox side: CHANGED on " + std::to_string(rows) + " tracks or playlists");
            detail::reportFieldChanges(out, changes, "rows");
        }
    }
    return result;
}

inline void printCompare(std::ostream &out, const CompareResult &result)
{
    out << "Planted cases\n=============\n";
    for (const CaseOutcome &c : result.cases) {
        out << "\n[" << c.planted.id << "] " << c.planted.title << "\n";
        out << "  track: " << c.planted.track << "\n";
        if (!c.counted()) {
            out << "  NOT PLANTED: " << c.planted.how << "\n";
            continue;
        }
        out << "  planted (" << c.planted.status << "): Engine " << c.planted.engine << "; rekordbox "
            << c.planted.rekordbox << "\n";
        out << "  VERDICT: " << verdictName(c.verdict()) << "\n";
        for (const ItemOutcome &io : c.items) {
            out << "    " << verdictName(io.verdict) << "  " << io.item.section << " " << io.item.field;
            if (io.item.member()) {
                out << " contains " << *io.item.member();
            }
            out << ": " << shown(io.before);
            if (io.verdict != Verdict::Kept) {
                out << " -> " << shown(io.after);
            }
            if (!io.item.rekordboxSection().empty()) {
                out << "   [rekordbox: " << shown(io.rekordbox)
                    << (io.after && io.after == io.rekordbox && io.verdict != Verdict::Kept ? ", Engine now matches it"
                                                                                           : "")
                    << "]";
                if (io.oneLibrary && io.oneLibrary != io.rekordbox) {
                    out << " [OneLibrary: " << shown(io.oneLibrary)
                        << (io.after && io.after == io.oneLibrary && io.verdict != Verdict::Kept
                                ? ", Engine now matches it"
                                : "")
                        << "]";
                }
            }
            out << "\n";
        }
        if (!c.alsoChanged.empty()) {
            out << "    also changed on this track:\n";
            for (const auto &line : c.alsoChanged) {
                out << "      " << line << "\n";
            }
        }
    }
    out << "\nEverything else\n===============\n";
    for (const auto &line : result.outside) {
        out << line << "\n";
    }
    const auto count = [&result](Verdict v) {
        const auto it = result.counts.find(v);
        return it == result.counts.end() ? 0 : it->second;
    };
    int total = 0;
    for (const auto &[v, n] : result.counts) {
        total += n;
    }
    out << "\nPROBE RESULT: " << total << " cases, " << count(Verdict::Overwritten) << " overwritten, "
        << count(Verdict::Kept) << " kept, " << count(Verdict::Deleted) << " deleted, " << count(Verdict::Added)
        << " added, " << count(Verdict::Merged) << " merged, " << result.notPlanted << " not planted\n";
}

}  // namespace seabass::probe
