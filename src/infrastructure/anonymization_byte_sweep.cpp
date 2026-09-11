// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/anonymization_byte_sweep.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>

namespace seabass::infrastructure
{

namespace fs = std::filesystem;

namespace
{

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// Everything above reads values back through this project's own readers,
// which is precisely the blind spot that let a real library ship inside a
// fixture this check had already passed. A reader returns live rows --
// exactly the rows the anonymizer just overwrote. It cannot return what
// sits in a SQLite freeblock, in a page the freelist has released, or in
// the slack DeviceSQL leaves behind when a row is removed. The committed
// 0.6-era fixture had 52 real artist names in engine/Database2/m.db and
// 403 in rekordbox/export.pdb, and reported zero problems.
//
// So this reads the bytes instead, and asks a blunter question: is there
// any run of text here that reads like prose and is not something we can
// account for? Accountable means a placeholder, a word out of the
// database's own schema, or a seam -- two anonymized values sit packed
// together and a byte scanner reads straight across the join between
// them, producing tokens like "bfc Albumec" that are an artefact of
// scanning rather than anything that was ever written.
//
// Like the rest of this file it is a tripwire, not a proof. It would miss
// a leaked value of one or two words that happen to be schema words. What
// it catches is the failure actually seen: a scrubber that updated rows
// and left the originals legible somewhere else in the same file.

bool hasVowel(const std::string &word)
{
    return word.find_first_of("aeiouy") != std::string::npos;
}

// Lowercased words of three letters or more, split on non-letters and on
// camelCase humps, so a schema identifier read out of the raw bytes
// ("PlaylistPath") yields the same words as the schema text that declared
// it ("playlist", "path").
std::vector<std::string> splitWords(const std::string &fragment)
{
    std::vector<std::string> words;
    std::string current;
    auto flush = [&current, &words]() {
        if (current.size() >= 3) {
            words.push_back(current);
        }
        current.clear();
    };
    for (size_t i = 0; i < fragment.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(fragment[i]);
        if (!std::isalpha(c)) {
            flush();
            continue;
        }
        // A capital starting a new hump ends the previous word, but only
        // when it is not part of a run of capitals ("SQLite" -> "lite").
        const bool startsHump = std::isupper(c) && !current.empty()
            && (std::islower(static_cast<unsigned char>(fragment[i - 1]))
                || (i + 1 < fragment.size() && std::islower(static_cast<unsigned char>(fragment[i + 1]))));
        if (startsHump) {
            flush();
        }
        current.push_back(static_cast<char>(std::tolower(c)));
    }
    flush();
    return words;
}

// Binary noise produces letter runs too. Real text has a vowel in every
// word and at least one word long enough to be a word.
bool looksLikeProse(const std::vector<std::string> &words)
{
    if (words.size() < 2) {
        return false;
    }
    if (std::none_of(words.begin(), words.end(), [](const std::string &w) { return w.size() >= 4; })) {
        return false;
    }
    return std::all_of(words.begin(), words.end(), hasVowel);
}

bool isAllHex(const std::string &word)
{
    return !word.empty() && word.size() <= 6 && std::all_of(word.begin(), word.end(), isHexDigit);
}

// The words every export legitimately contains: SQL the schema is written
// in, and the label half of this project's own placeholders.
const std::set<std::string> &staticVocabulary()
{
    static const std::set<std::string> vocabulary = {
        // SQL, as it appears in a schema read out of raw bytes.
        "create", "table", "index", "trigger", "view", "unique", "primary", "key", "autoincrement",
        "integer", "text", "real", "blob", "null", "not", "default", "references", "foreign",
        "cascade", "delete", "update", "insert", "select", "from", "where", "and", "begin", "end",
        "for", "each", "row", "when", "new", "old", "raise", "abort", "fail", "ignore", "replace",
        "union", "all", "distinct", "order", "group", "having", "limit", "offset", "into", "values",
        "set", "collate", "nocase", "asc", "desc", "constraint", "check", "exists", "case", "then",
        "else", "temp", "temporary", "without", "rowid", "conflict", "transaction", "commit",
        "rollback", "pragma", "vacuum", "analyze", "datetime", "varchar", "boolean", "numeric",
        "char", "date", "time", "timestamp", "decimal", "float", "double", "bigint", "smallint",
        "are", "allowed", "like", "between", "ifnull", "coalesce", "cast", "strftime", "unsigned",
        // The label half of anonymizationPlaceholder() and friends.
        "track", "album", "comment", "artist", "genre", "label", "playlist", "folder", "none",
        "unknown", "mpthree", "mp3", "m4a", "flac", "wav", "aiff", "aif",
        // "SQLite format 3", the header every SQLite file opens with.
        "sqlite", "lite", "format",
        // The fixed column header AnonymizeLibrary writes at the top of
        // files.tsv: "# relative path\tsize in bytes".
        "relative", "path", "size", "bytes",
    };
    return vocabulary;
}

// A word we can account for: known, a placeholder's hash, or a known word
// with an adjacent value's bytes stuck to one end of it.
bool isAccountableWord(const std::string &word, const std::set<std::string> &vocabulary)
{
    if (vocabulary.count(word) > 0 || isAllHex(word)) {
        return true;
    }
    for (const auto &known : vocabulary) {
        if (known.size() < 4) {
            continue;
        }
        if (word.compare(0, known.size(), known) == 0
            || (word.size() >= known.size()
                && word.compare(word.size() - known.size(), known.size(), known) == 0)) {
            return true;
        }
    }
    // A hash with a stray letter or two from the neighbouring value: strip
    // the hex from either end and see whether anything of a word is left.
    size_t from = 0;
    while (from < word.size() && isHexDigit(word[from])) {
        ++from;
    }
    size_t to = word.size();
    while (to > from && isHexDigit(word[to - 1])) {
        --to;
    }
    return to - from < 3;
}

// Every identifier the database itself declares -- table and column
// names, and the text of its triggers and views, which in the Engine
// schema includes English RAISE messages like "Recycling deleted track".
// Harvested from the file rather than hand-listed, so a schema this
// project has never seen cannot start failing exports.
std::set<std::string> schemaVocabulary(const fs::path &file)
{
    std::set<std::string> vocabulary;
    sqlite3 *db = nullptr;
    const std::string uri = "file:" + file.string() + "?mode=ro&immutable=1";
    if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return vocabulary;
    }
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT name, sql FROM sqlite_master", -1, &statement, nullptr) == SQLITE_OK) {
        while (sqlite3_step(statement) == SQLITE_ROW) {
            for (int column = 0; column < 2; ++column) {
                const unsigned char *text = sqlite3_column_text(statement, column);
                if (text == nullptr) {
                    continue;
                }
                for (const auto &word : splitWords(reinterpret_cast<const char *>(text))) {
                    vocabulary.insert(word);
                }
            }
        }
    }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return vocabulary;
}

bool isSqliteFile(const std::string &bytes)
{
    static const std::string magic = "SQLite format 3";
    return bytes.size() >= magic.size() && bytes.compare(0, magic.size(), magic) == 0;
}

// Printable runs, in the three encodings these files actually use: UTF-8
// for SQLite, UTF-16LE for DeviceSQL's text, UTF-16BE for the paths the
// analysis files embed.
std::vector<std::string> printableRuns(const std::string &bytes)
{
    std::vector<std::string> runs;
    constexpr size_t MinimumRun = 6;
    auto isPrintable = [](char c) { return c >= 0x20 && c <= 0x7e; };

    // textAt/companionAt are offsets within each stride-sized unit;
    // companionAt == stride means there is no companion byte to require a
    // NUL in, i.e. the 8-bit pass. Running both UTF-16 phases covers a
    // string that starts at an odd offset as well as an even one.
    auto collect = [&](size_t textAt, size_t companionAt, size_t stride) {
        std::string current;
        for (size_t base = 0; base + stride <= bytes.size(); base += stride) {
            const char c = bytes[base + textAt];
            const bool printable = isPrintable(c)
                && (companionAt == stride || bytes[base + companionAt] == '\0');
            if (printable) {
                current.push_back(c);
                continue;
            }
            if (current.size() >= MinimumRun) {
                runs.push_back(current);
            }
            current.clear();
        }
        if (current.size() >= MinimumRun) {
            runs.push_back(current);
        }
    };
    collect(0, 1, 1);  // UTF-8 / ASCII
    collect(0, 1, 2);  // UTF-16LE
    collect(1, 0, 2);  // UTF-16BE
    return runs;
}

// Runs of two or more words of three-plus letters, joined by a single
// space, apostrophe or hyphen -- the shape of a title or an artist name.
std::vector<std::string> proseFragments(const std::string &run)
{
    std::vector<std::string> fragments;
    auto isAlpha = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };
    auto isJoiner = [](char c) { return c == ' ' || c == '\'' || c == '-'; };
    size_t i = 0;
    while (i < run.size()) {
        if (!isAlpha(run[i])) {
            ++i;
            continue;
        }
        const size_t begin = i;
        size_t end = i;
        int words = 0;
        while (i < run.size() && isAlpha(run[i])) {
            const size_t wordBegin = i;
            while (i < run.size() && isAlpha(run[i])) {
                ++i;
            }
            if (i - wordBegin < 3) {
                break;
            }
            ++words;
            end = i;
            if (i < run.size() && isJoiner(run[i]) && i + 1 < run.size() && isAlpha(run[i + 1])) {
                ++i;
                continue;
            }
            break;
        }
        if (words >= 2) {
            fragments.push_back(run.substr(begin, end - begin));
        }
    }
    return fragments;
}


}  // namespace

std::vector<std::string> readableTextInRawBytes(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return {};
    }

    std::set<std::string> vocabulary = staticVocabulary();
    if (isSqliteFile(bytes)) {
        for (const auto &word : schemaVocabulary(file)) {
            vocabulary.insert(word);
        }
    }

    std::set<std::string> unaccounted;
    for (const auto &run : printableRuns(bytes)) {
        for (const auto &fragment : proseFragments(run)) {
            const auto words = splitWords(fragment);
            if (!looksLikeProse(words)) {
                continue;
            }
            if (std::all_of(words.begin(), words.end(),
                            [&vocabulary](const std::string &w) { return isAccountableWord(w, vocabulary); })) {
                continue;
            }
            unaccounted.insert(fragment);
        }
    }
    return {unaccounted.begin(), unaccounted.end()};
}

}  // namespace seabass::infrastructure
