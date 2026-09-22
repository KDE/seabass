// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Rewriting the My Tag names in an exportExt.pdb.
//
// Issue #1: the anonymizer DELETES exportExt.pdb, because My Tag names
// are free text a DJ typed and nothing could anonymize them. So no
// committed fixture carries one, and nothing Seabass ever does with My
// Tags can be exercised against real-shaped data. This is the piece that
// was missing: a byte-length-preserving rewrite of those names, so the
// file can be kept instead of dropped.
//
// exportExt.pdb is the same container as export.pdb with a different
// table-type enum, and its rows are reached through body_ext() rather
// than body(). The kaitai parser takes that as a construction flag, and
// getting it wrong does not fail loudly: it simply finds no rows. That
// is why this test asserts a COUNT before and after rather than only
// that nothing threw.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

#include "scratch_path.hpp"

using seabass::infrastructure::rekordbox::PdbRowWriter;
using Pdb = rekordbox_pdb_t;
namespace fs = std::filesystem;

namespace
{

// Every My Tag/category name in the file, read back through the parser
// rather than through the writer, so the check does not lean on the code
// it is checking.
std::vector<std::string> tagNamesIn(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string buffer = ss.str();
    std::istringstream iss(buffer);
    kaitai::kstream ks(&iss);
    Pdb pdb(true, &ks);
    std::vector<std::string> names;
    for (const auto &t : *pdb.tables()) {
        if (t->type_ext() != Pdb::PAGE_TYPE_EXT_TAGS) {
            continue;
        }
        auto pageRef = t->first_page();
        for (;;) {
            auto page = pageRef->body();
            if (page->is_data_page()) {
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (!row->present()) {
                            continue;
                        }
                        if (auto *tag = dynamic_cast<Pdb::tag_row_t *>(row->body_ext())) {
                            names.push_back(seabass::infrastructure::rekordbox::sqlText(tag->name()));
                        }
                    }
                }
            }
            if (pageRef->index() == t->last_page()->index()) {
                break;
            }
            pageRef = page->next_page();
        }
    }
    return names;
}

}  // namespace

int main()
{
    const fs::path source = fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "exportExt.pdb";
    if (!fs::is_regular_file(source)) {
        // Deliberately a failure, not a skip. This test exists because
        // no fixture carried one; a run that quietly passes without the
        // file would restore exactly the gap issue #1 is about.
        std::cout << "missing fixture: " << source.string() << "\n"
                  << "An exportExt.pdb is needed for this test. It is produced from a real stick by\n"
                  << "the anonymizer, which now keeps the file instead of deleting it.\n";
        return 1;
    }

    const fs::path scratch = seabass::testing::scratchRoot() / "pdb-tag-names";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const fs::path working = scratch / "exportExt.pdb";
    fs::copy_file(source, working);

    const std::vector<std::string> before = tagNamesIn(working);
    std::cout << "names before: " << before.size() << "\n";
    // The fixture has to actually contain tags, or everything below
    // passes by finding nothing -- which is how a rewrite that does
    // nothing looks identical to one that works.
    assert(before.size() > 1 && "the fixture must carry My Tags for this to mean anything");

    // NOT "Tag N", which is what the anonymizer writes and therefore
    // what the committed fixture already holds: rewriting those to
    // themselves is an identity edit, and every assertion below would
    // pass over a writer that did nothing at all. The placeholder has to
    // be something the fixture cannot already contain.
    int rewritten = 0;
    {
        PdbRowWriter writer(working.string(), PdbRowWriter::Format::ExportExt);
        rewritten = writer.overwriteAllTagNames([](size_t i) { return "Zzzz" + std::to_string(i + 1); });
        std::cout << "rewritten: " << rewritten << "\n";
        assert(rewritten == static_cast<int>(before.size()));
        assert(writer.commit());
    }

    const std::vector<std::string> after = tagNamesIn(working);
    assert(after.size() == before.size() && "the rewrite must not add or lose a row");

    // Not one original name survives anywhere in the file, including in
    // the bytes the parser does not reach: a name that stayed behind in
    // slack would be exactly the leak the deletion was avoiding.
    std::ifstream in(working, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string raw = ss.str();
    for (const std::string &name : before) {
        if (name.size() < 4) {
            continue;  // too short to be identifying, and prone to chance matches
        }
        assert(raw.find(name) == std::string::npos && "an original tag name survived in the file");
    }

    // The replacements are there. NOT asserted distinct: the rewrite
    // preserves each field's byte length, so "Tag 12" written over a
    // four-byte name like "Beat" is truncated to "Tag ", and several
    // short tags collapse onto the same placeholder. That is inherent to
    // a length-preserving rewrite and the existing Album and Genre
    // placeholders behave the same way. What matters for a fixture is
    // that no real name survives, which is asserted above.
    //
    // It does leak each name's LENGTH, which is the same trade the rest
    // of the anonymizer already makes and is recorded here so nobody
    // reads the omission as an oversight.
    const std::set<std::string> distinct(after.begin(), after.end());
    assert(!distinct.empty());
    for (const std::string &name : after) {
        assert(name.rfind("Zz", 0) == 0 && "every name is a placeholder now");
    }
    std::cout << "after: " << distinct.size() << " distinct placeholder(s) over " << after.size()
              << " rows, e.g. \"" << after.front() << "\"\n";

    // Opened as the wrong format, the same file yields nothing. Asserted
    // because it is the failure mode of this whole area: a mismatched
    // flag finds no rows and reports success.
    {
        PdbRowWriter wrongFormat(working.string());
        assert(wrongFormat.overwriteAllTagNames([](size_t) { return "x"; }) == 0);
    }

    fs::remove_all(scratch);
    std::cout << "pdb_tag_names_test: ok\n";
    return 0;
}
