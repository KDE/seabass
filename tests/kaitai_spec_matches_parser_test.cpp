// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The vendored Kaitai specs and the parser generated from them describe
// the same thing.
//
// specs/*.ksy and src/infrastructure/rekordbox/generated/* both come
// from crate-digger, and they arrive as two separate files that nothing
// forces to agree. If a spec is updated without regenerating the parser,
// the code keeps compiling and keeps working -- it simply stops being
// able to see whatever the new spec added, silently.
//
// This exists because that question was answered wrong. While tracking
// down a bug in tag_row, a grep over part of the generated class came
// back without ofs_unknown_near and the conclusion drawn was that the
// spec was ahead of the parser. It is not: the accessor is there, one
// line past the end of the range that was read. The claim went into a
// commit message and into two code comments before anyone checked it.
//
// So: a check rather than a habit, the same reasoning as
// no_em_dashes_test. It answers the question by comparing the two files
// in full, in both directions, and it is cheap enough to run always.
//
// What it compares is FIELD NAMES per type, not types or semantics. A
// parser regenerated from a spec whose field changed type, or whose
// meaning changed, passes this. It catches the failure that actually
// happens -- a field added to the spec and absent from the parser -- and
// nothing subtler.
//
// THE MINIMUM COUNTS IN main() ARE LOAD-BEARING. They look like
// arbitrary constants and they are the only thing standing between this
// test and passing by reading nothing: every assertion here is of the
// form "everything in set A is in set B", which is trivially true when A
// is empty, and A comes out of a regex over a file whose shape this test
// does not control. Reindent a spec, or change the code generator's
// output style, and the extraction quietly returns nothing and the run
// reports agreement.
//
// That is not hypothetical. The floors were a single total across both
// spec/parser pairs at first; reindenting the pdb spec dropped it to
// zero types, the anlz pair's own counts covered for it, and the test
// passed while comparing one file instead of two. Per pair now. If you
// change the extraction, change these to match what it really finds --
// do not lower them to make a run go green.

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

std::vector<std::string> linesOf(const fs::path &file)
{
    std::ifstream in(file);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

std::string textOf(const fs::path &file)
{
    std::ifstream in(file);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct SpecType
{
    std::set<std::string> fields;  // seq ids and instance names: what the parser must expose
    std::set<std::string> params;  // constructor parameters: exposed too, but declared elsewhere
};

// The `types:` block of a .ksy, by name. A hand-rolled reader of the
// subset these two files use rather than a YAML dependency: they are
// vendored and regular, and a real parser would be more code than the
// thing it checks.
std::map<std::string, SpecType> typesIn(const fs::path &ksy)
{
    std::map<std::string, SpecType> types;
    const std::regex typeLine(R"(^  ([a-z0-9_]+):\s*$)");
    const std::regex sectionLine(R"(^    ([a-z-]+):\s*$)");
    const std::regex seqId(R"(^      - id: ([a-z0-9_]+)\s*$)");
    const std::regex instanceName(R"(^      ([a-z0-9_]+):\s*$)");

    std::string current;
    std::string section;
    for (const std::string &line : linesOf(ksy)) {
        std::smatch m;
        if (std::regex_match(line, m, typeLine)) {
            current = m[1];
            types[current];
            section.clear();
            continue;
        }
        if (current.empty()) {
            continue;
        }
        if (std::regex_match(line, m, sectionLine)) {
            section = m[1];
            continue;
        }
        if (section == "seq" || section == "params") {
            if (std::regex_match(line, m, seqId)) {
                (section == "params" ? types[current].params : types[current].fields).insert(m[1]);
            }
        } else if (section == "instances") {
            if (std::regex_match(line, m, instanceName)) {
                types[current].fields.insert(m[1]);
            }
        }
    }
    return types;
}

// Every name the generated class for `type` exposes as a member
// function, whether inline (`x() const { return m_x; }`) or declared
// (`uint16_t x();` for a lazy instance). Returns false in `found` when
// the class is not in the header at all, which is itself a finding --
// and, left unchecked, is how a comparison like this passes by comparing
// nothing.
std::set<std::string> accessorsOf(const std::string &header, const std::string &type, bool &found)
{
    const std::regex classBody("class " + type + R"(_t : public kaitai::kstruct \{([\s\S]*?)\n    \};)");
    std::smatch m;
    found = std::regex_search(header, m, classBody);
    std::set<std::string> names;
    if (!found) {
        return names;
    }
    const std::string body = m[1];
    const std::regex inlineGetter(R"(\b([a-z0-9_]+)\(\) const \{ return m_)");
    const std::regex declaredGetter(R"(\n\s+[A-Za-z_][\w:*<>, ]*\s+([a-z0-9_]+)\(\);)");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), inlineGetter); it != std::sregex_iterator(); ++it) {
        names.insert((*it)[1]);
    }
    for (auto it = std::sregex_iterator(body.begin(), body.end(), declaredGetter); it != std::sregex_iterator();
         ++it) {
        names.insert((*it)[1]);
    }
    return names;
}

struct Result
{
    int typesCompared = 0;
    int fieldsCompared = 0;
    std::vector<std::string> problems;
};

// Enums live in the spec's `types:`-adjacent `enums:` block and produce
// no class, so a spec name with no class is only a finding when the spec
// actually described fields for it.
Result compare(const fs::path &ksy, const fs::path &headerPath)
{
    Result result;
    const std::string header = textOf(headerPath);
    for (const auto &[name, spec] : typesIn(ksy)) {
        if (spec.fields.empty() && spec.params.empty()) {
            continue;  // an enum or a doc-only stanza: no class to compare
        }
        bool found = false;
        const std::set<std::string> accessors = accessorsOf(header, name, found);
        if (!found) {
            result.problems.push_back(ksy.filename().string() + " describes type '" + name
                                      + "' but the generated parser has no class for it");
            continue;
        }
        ++result.typesCompared;
        for (const std::string &field : spec.fields) {
            ++result.fieldsCompared;
            if (accessors.count(field) == 0) {
                result.problems.push_back(name + "." + field + " is in " + ksy.filename().string()
                                          + " but the generated parser does not expose it -- the parser needs "
                                            "regenerating from the spec");
            }
        }
        // The other direction. A parser field the spec does not mention
        // means the SPEC is the stale one, which is the case that would
        // otherwise be read as "all good".
        for (const std::string &accessor : accessors) {
            if (accessor.rfind('_', 0) == 0) {
                continue;  // _unnamed3, _root, _parent, _io: kaitai's own
            }
            if (spec.fields.count(accessor) > 0 || spec.params.count(accessor) > 0) {
                continue;
            }
            result.problems.push_back(name + "." + accessor + " is in the generated parser but not in "
                                      + ksy.filename().string() + " -- the spec is behind the parser");
        }
    }
    return result;
}

}  // namespace

int main()
{
    const fs::path root(SEABASS_SOURCE_DIR);
    const fs::path generated = root / "src" / "infrastructure" / "rekordbox" / "generated";

    struct Pair
    {
        fs::path ksy;
        fs::path header;
        int minTypes;
        int minFields;
    };
    // Floors are PER PAIR, not on the total, and that distinction was
    // learned the hard way: with one floor over both specs, reindenting
    // the pdb spec dropped it to zero types and the anlz pair's own
    // counts still cleared the bar. A whole spec can fall out of the
    // comparison and be reported as agreement. Each pair now has to
    // carry its own weight. The numbers are well under the real ones
    // (23/140 and 21/89) and exist only to fail loudly if this test's
    // reading of a file breaks.
    const std::vector<Pair> pairs = {
        {root / "specs" / "rekordbox_pdb.ksy", generated / "rekordbox_pdb.h", 18, 100},
        {root / "specs" / "rekordbox_anlz.ksy", generated / "rekordbox_anlz.h", 15, 60},
    };

    int totalTypes = 0;
    int totalFields = 0;
    std::vector<std::string> problems;
    for (const Pair &pair : pairs) {
        if (!fs::is_regular_file(pair.ksy) || !fs::is_regular_file(pair.header)) {
            std::cerr << "missing " << pair.ksy << " or " << pair.header << "\n";
            return 1;
        }
        const Result result = compare(pair.ksy, pair.header);
        std::cout << pair.ksy.filename().string() << ": " << result.typesCompared << " type(s), "
                  << result.fieldsCompared << " field(s) compared\n";
        if (result.typesCompared < pair.minTypes || result.fieldsCompared < pair.minFields) {
            std::cerr << "extracted too little from " << pair.ksy.filename().string() << " to be meaningful: "
                      << result.typesCompared << " type(s), " << result.fieldsCompared << " field(s), expected at "
                      << "least " << pair.minTypes << " and " << pair.minFields << ".\nThe spec or the generated "
                      << "header has changed shape and this test's own reading of them needs fixing. This is NOT "
                      << "evidence that they agree.\n";
            return 1;
        }
        totalTypes += result.typesCompared;
        totalFields += result.fieldsCompared;
        problems.insert(problems.end(), result.problems.begin(), result.problems.end());
    }

    if (!problems.empty()) {
        std::cerr << "\nthe vendored spec and the generated parser disagree:\n";
        for (const std::string &problem : problems) {
            std::cerr << "  - " << problem << "\n";
        }
        std::cerr << "\nRegenerate the parser with kaitai-struct-compiler against the vendored spec, or update\n"
                     "the spec, so that src/infrastructure/rekordbox/generated/ and specs/ come from one\n"
                     "version of crate-digger.\n";
        return 1;
    }

    // Named on purpose. This is the field a bug hunt concluded was
    // missing from the parser, on the strength of a grep over part of
    // the class; it is there, and the conclusion reached print. If the
    // general comparison above ever regresses into passing vacuously,
    // this one still has to hold.
    {
        const std::string pdb = textOf(generated / "rekordbox_pdb.h");
        assert(pdb.find("ofs_unknown_near()") != std::string::npos
               && "tag_row.ofs_unknown_near must be exposed by the generated parser");
    }

    std::cout << "kaitai_spec_matches_parser_test: ok (" << totalTypes << " types, " << totalFields
              << " fields)\n";
    return 0;
}
