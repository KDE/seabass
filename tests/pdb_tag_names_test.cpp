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
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

#include "infrastructure/paths/utf8_path.hpp"
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
    const fs::path source = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "exportExt.pdb";
    if (!fs::is_regular_file(source)) {
        // Deliberately a failure, not a skip. This test exists because
        // no fixture carried one; a run that quietly passes without the
        // file would restore exactly the gap issue #1 is about.
        std::cout << "missing fixture: " << seabass::pathToUtf8(source) << "\n"
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

    // NOT "Tag NNN", which is what both writers of this file produce --
    // the anonymizer's placeholder() and tools/anonymize_export_ext,
    // which now agree on the zero-padded form after a review found them
    // disagreeing. The committed fixture therefore already holds those,
    // and rewriting them to themselves is an identity edit that every
    // assertion below survives. The placeholder has to be something the
    // fixture cannot already contain.
    int rewritten = 0;
    {
        PdbRowWriter writer(seabass::pathToUtf8(working), PdbRowWriter::Format::ExportExt);
        int leftAloneHere = 0;
        rewritten = writer.overwriteAllTagNames([](size_t i) { return "Zzzz" + std::to_string(i + 1); },
                                               &leftAloneHere);
        assert(leftAloneHere == 0 && "every present row was rewritten");
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

    // Clearing free space must not eat a LIVE row byte.
    //
    // zeroUnusedSpace() keeps what it can account for and zeroes the
    // rest, so a keep-range that misses a field destroys it silently.
    // The first version of the tag branch did exactly that: a tag_row's
    // fixed header is 31 bytes, through ofs_unknown_near at offset 30,
    // and it stopped at 30 -- losing that byte and the 0x03 empty string
    // it points at. Two bytes per row, 56 across this fixture.
    //
    // Nothing above could see it. The names still read back, this test
    // passed, and the file still reparsed.
    //
    // A correction, because the first version of this comment got it
    // wrong and the wrong version is in the history: it claimed the
    // generated parser had no accessor for ofs_unknown_near and that a
    // field-level comparison therefore could not have caught this. The
    // parser does expose it -- the accessor was one line past the end of
    // the range that was grepped -- so a field-level check WOULD have
    // worked. kaitai_spec_matches_parser_test now checks that claim
    // instead of anyone asserting it.
    //
    // The byte-level check below is still the right one, for a plainer
    // reason: it does not depend on knowing which fields a row has. A
    // check written from the offsets is a check written from the same
    // understanding that produced the bug.
    //
    // The invariant: after the names are rewritten, every remaining
    // non-zero byte in this file is live. The fixture is produced by
    // tools/anonymize_export_ext from a stick whose tag pages carry no
    // slack residue, so clearing free space must not turn a single
    // non-zero byte into a zero. If this ever fires on a regenerated
    // fixture, check whether the new stick genuinely has residue before
    // assuming the keep-ranges are wrong again.
    {
        const fs::path slack = scratch / "slack.pdb";
        fs::copy_file(source, slack, fs::copy_options::overwrite_existing);

        auto bytesOf = [](const fs::path &p) {
            std::ifstream in(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        const std::string before = bytesOf(slack);
        const size_t namesBefore = tagNamesIn(slack).size();

        int cleared = 0;
        {
            PdbRowWriter writer(seabass::pathToUtf8(slack), PdbRowWriter::Format::ExportExt);
            cleared = writer.zeroUnusedSpace();
            assert(writer.commit());
        }

        const std::string after = bytesOf(slack);
        assert(before.size() == after.size() && "clearing free space must not resize the file");
        size_t liveBytesLost = 0;
        for (size_t i = 0; i < before.size(); ++i) {
            if (before[i] != '\0' && after[i] == '\0') {
                ++liveBytesLost;
            }
        }
        std::cout << "free space: " << cleared << " byte(s) cleared, " << liveBytesLost << " live byte(s) lost\n";
        assert(liveBytesLost == 0 && "clearing free space zeroed a byte that was in use");

        // And it did not pass by doing nothing: the rows are all still
        // there and still readable afterwards.
        assert(tagNamesIn(slack).size() == namesBefore);
        assert(namesBefore > 1);
    }

    // A row the rewrite cannot touch is REPORTED, not silently left.
    //
    // Every `continue` in the write loop leaves that row's real My Tag
    // name exactly where it was, and the return value counts rows
    // rewritten -- so 27 of 28 reads as a healthy positive number while
    // one name a DJ typed goes out inside a file the anonymiser has
    // just called scrubbed. The caller cannot see that from the return
    // alone, which is why there is an out-parameter for it.
    //
    // Provoked with a placeholder the field cannot represent: non-ASCII
    // is refused by overwriteDeviceSqlStringInPlace(), so every row is
    // skipped and none of the names change.
    {
        const fs::path refused = scratch / "refused.pdb";
        fs::copy_file(source, refused, fs::copy_options::overwrite_existing);
        const std::vector<std::string> namesBefore = tagNamesIn(refused);

        PdbRowWriter writer(seabass::pathToUtf8(refused), PdbRowWriter::Format::ExportExt);
        int leftAlone = -1;
        const int rewritten = writer.overwriteAllTagNames([](size_t) { return std::string("Café"); }, &leftAlone);
        std::cout << "unrepresentable placeholder: " << rewritten << " rewritten, " << leftAlone
                  << " left alone\n";
        assert(rewritten == 0 && "a placeholder the field cannot represent must not be written");
        assert(leftAlone == static_cast<int>(namesBefore.size())
               && "and every row it could not do has to be counted, not dropped on the floor");

        // The names really are untouched, read back through the parser:
        // this is the state that would have shipped.
        const std::vector<std::string> after = tagNamesIn(refused);
        assert(after == namesBefore);
    }

    // Opened as the wrong format, the same file yields nothing. Asserted
    // because it is the failure mode of this whole area: a mismatched
    // flag finds no rows and reports success.
    {
        PdbRowWriter wrongFormat(seabass::pathToUtf8(working));
        int leftAloneWrongFormat = -1;
        assert(wrongFormat.overwriteAllTagNames([](size_t) { return "x"; }, &leftAloneWrongFormat) == 0);
        assert(leftAloneWrongFormat == 0 && "a writer of the wrong format left no tag row behind, it saw none");
    }

    // Best-effort, and retried rather than asserted: every assertion this
    // test exists for has already passed by this point, and a temp file
    // Windows Defender's real-time scanner still has open for a moment
    // -- confirmed directly, "the process cannot access the file because
    // it is being used by another process" on a .pdb this test itself
    // had just finished writing -- is not this test failing, it is this
    // test's own cleanup racing an antivirus scan of the scratch
    // directory it wrote several PDB files into. A few retries ride out
    // that window; std::error_code means a cleanup that still cannot
    // land after that is a leftover temp file, not a crash.
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::error_code ec;
        fs::remove_all(scratch, ec);
        if (!ec || !fs::exists(scratch)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "pdb_tag_names_test: ok\n";
    return 0;
}
