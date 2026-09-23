// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <stdexcept>
#include <iostream>

#include "infrastructure/rekordbox/anlz_cue_codec.hpp"

using namespace seabass::infrastructure::rekordbox;

namespace
{

// Big-endian appenders, the same order the format uses everywhere.
void appendU32(std::string &out, uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

void appendU16(std::string &out, uint16_t value)
{
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

std::string fromHex(const std::string &hex)
{
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

}  // namespace

int main()
{
    // Real PCO2 section captured from testdata/PIONEER-rb7-stick's
    // "Another Brick In The Wall" (track id=14): one hot cue, no color,
    // time=9875ms. Ground truth confirmed against seabass's own scan
    // output for this exact track earlier this session.
    {
        auto section =
            fromHex("50434f320000001400000040000000010001000050435032000000100000002c00000001010003e"
                    "8000026930000000000030132000000000000000000000000");
        auto cues = AnlzCueCodec::decodeHotCues(section);
        assert(cues.size() == 1);
        assert(cues[0].hotCueNumber == 1);
        assert(cues[0].timeMs == 9875);
        assert(!cues[0].color.has_value());
        std::cout << "case 1 (decode real no-color entry) OK\n";
    }

    // Real PCO2 section from "Voices In My Head" (track id=245): one hot
    // cue with color #FF0017 (255, 0, 23), time=11333ms.
    {
        auto section = fromHex(
            "50434f32000000140000006c000000010001000050435032000000100000005800000001010003e800002c"
            "45ffffffff000101a80000000000000000000000002bff001700000000000000000000000000000000000000"
            "000000000000000000000000000000000000000000");
        auto cues = AnlzCueCodec::decodeHotCues(section);
        assert(cues.size() == 1);
        assert(cues[0].hotCueNumber == 1);
        assert(cues[0].timeMs == 11333);
        assert(cues[0].color.has_value());
        auto [r, g, b] = *cues[0].color;
        assert(r == 255 && g == 0 && b == 23);
        std::cout << "case 2 (decode real colored entry) OK\n";
    }

    // Round trip: encode two synthetic cues (one plain, one colored), then
    // decode the result back and confirm it matches what we put in.
    {
        std::vector<RawHotCueEntry> cues(2);
        cues[0].hotCueNumber = 1;
        cues[0].timeMs = 5000;
        cues[1].hotCueNumber = 2;
        cues[1].timeMs = 12345;
        cues[1].color = std::make_tuple<uint8_t, uint8_t, uint8_t>(10, 20, 30);

        auto encoded = AnlzCueCodec::encodeHotCues(cues);
        auto decoded = AnlzCueCodec::decodeHotCues(encoded);

        assert(decoded.size() == 2);
        assert(decoded[0].hotCueNumber == 1);
        assert(decoded[0].timeMs == 5000);
        assert(!decoded[0].color.has_value());
        assert(decoded[1].hotCueNumber == 2);
        assert(decoded[1].timeMs == 12345);
        assert(decoded[1].color.has_value());
        auto [r, g, b] = *decoded[1].color;
        assert(r == 10 && g == 20 && b == 30);
        std::cout << "case 3 (encode/decode round trip) OK\n";
    }

    // Real PCO2 section from "Voices In My Head" (track id=245) -- same
    // track as the hot-cue examples above -- but the memory-cues list
    // (type=0), currently empty on this real file.
    {
        auto section = fromHex("50434f3200000014000000140000000000000000");
        auto cues = AnlzCueCodec::decodeHotCues(section, CueListTypeMemory);
        assert(cues.empty());
        std::cout << "case 4 (decode real empty memory-cues section) OK\n";
    }

    // Round trip a memory-cues list (type=0): entries carry hotCueNumber=0
    // (memory cues have no hot-cue slot), same as a real one would.
    {
        std::vector<RawHotCueEntry> cues(2);
        cues[0].hotCueNumber = 0;
        cues[0].timeMs = 7000;
        cues[1].hotCueNumber = 0;
        cues[1].timeMs = 30000;
        cues[1].color = std::make_tuple<uint8_t, uint8_t, uint8_t>(1, 2, 3);

        auto encoded = AnlzCueCodec::encodeHotCues(cues, CueListTypeMemory);
        auto decoded = AnlzCueCodec::decodeHotCues(encoded, CueListTypeMemory);

        assert(decoded.size() == 2);
        assert(decoded[0].hotCueNumber == 0);
        assert(decoded[0].timeMs == 7000);
        assert(!decoded[0].color.has_value());
        assert(decoded[1].hotCueNumber == 0);
        assert(decoded[1].timeMs == 30000);
        assert(decoded[1].color.has_value());

        // Decoding this same section as a hot-cues list must find nothing
        // (wrong `type`, and hot_cue==0 entries are excluded anyway).
        auto asHot = AnlzCueCodec::decodeHotCues(encoded, CueListTypeHot);
        assert(asHot.empty());
        std::cout << "case 5 (memory-cues list round trip, hotCueNumber=0) OK\n";
    }

    // A loop encodes as the spec's loop entry type with its out point,
    // and decodes back as one.
    {
        RawHotCueEntry loop;
        loop.hotCueNumber = 3;
        loop.timeMs = 42000;
        loop.isLoop = true;
        loop.loopEndMs = 46000;
        loop.color = std::make_tuple(uint8_t{0}, uint8_t{255}, uint8_t{0});
        RawHotCueEntry point;
        point.hotCueNumber = 4;
        point.timeMs = 50000;
        auto encoded = AnlzCueCodec::encodeHotCues({loop, point});
        auto decoded = AnlzCueCodec::decodeHotCues(encoded);
        assert(decoded.size() == 2);
        assert(decoded[0].isLoop && decoded[0].loopEndMs == 46000 && decoded[0].timeMs == 42000);
        assert(static_cast<unsigned char>(decoded[0].rawBytes[16]) == 2);
        assert(!decoded[1].isLoop && decoded[1].loopEndMs == 0);
        assert(static_cast<unsigned char>(decoded[1].rawBytes[16]) == 1);
        std::cout << "case 6 (a loop round-trips with its out point) OK\n";
    }

    // An entry read from a file is carried back byte for byte when it
    // is re-encoded: the real coloured entry of case 2, with its legacy
    // colour id and every byte this codec does not model, comes out of
    // a decode/encode round trip identical.
    {
        auto section = fromHex(
            "50434f32000000140000006c000000010001000050435032000000100000005800000001010003e800002c"
            "45ffffffff000101a80000000000000000000000002bff001700000000000000000000000000000000000000"
            "000000000000000000000000000000000000000000");
        auto cues = AnlzCueCodec::decodeHotCues(section);
        assert(cues.size() == 1 && !cues[0].rawBytes.empty());
        auto encoded = AnlzCueCodec::encodeHotCues(cues);
        assert(encoded == section);
        // Clearing rawBytes falls back to a fresh encode, which differs
        // in the bytes the model does not carry (proving the carry-over
        // is what preserved them above).
        cues[0].rawBytes.clear();
        assert(AnlzCueCodec::encodeHotCues(cues) != section);
        // A torn raw entry is refused rather than written.
        auto torn = AnlzCueCodec::decodeHotCues(section);
        torn[0].rawBytes.pop_back();
        bool refused = false;
        try {
            AnlzCueCodec::encodeHotCues(torn);
        } catch (const std::runtime_error &) {
            refused = true;
        }
        assert(refused);
        std::cout << "case 7 (an unchanged entry is written back byte for byte) OK\n";
    }

    // ---- The edges (#7, Tier 2) ---------------------------------------
    //
    // This codec was validated against real rekordbox files, which is the
    // right way to build it and leaves one gap: real files from one DJ's
    // library do not carry the edges. Everything below is a shape that
    // did not happen to be in those seven entries.

    // An empty list, encoded rather than decoded. Case 4 decodes a real
    // empty memory-cues section; nothing had ever produced one. It
    // happens for real the moment somebody removes their last cue from
    // a track, which is what Clean Up Stray Cues does.
    {
        const std::string encoded = AnlzCueCodec::encodeHotCues({}, CueListTypeMemory);
        assert(AnlzCueCodec::decodeHotCues(encoded, CueListTypeMemory).empty());
        // And it is the same twenty bytes rekordbox itself writes, which
        // is the one empty section that has been captured.
        assert(encoded == fromHex("50434f3200000014000000140000000000000000"));
        const std::string emptyHot = AnlzCueCodec::encodeHotCues({}, CueListTypeHot);
        assert(AnlzCueCodec::decodeHotCues(emptyHot, CueListTypeHot).empty());
        std::cout << "case 8 (an emptied list encodes to the same bytes rekordbox writes) OK\n";
    }

    // Black. A cue coloured (0,0,0) must not come back as a cue with no
    // colour: presence is carried by the entry's length, not by the
    // bytes being nonzero, and a codec that confused the two would
    // silently drop the colour off every black pad.
    {
        std::vector<RawHotCueEntry> cues(3);
        cues[0].hotCueNumber = 1;
        cues[0].timeMs = 1000;
        cues[0].color = std::make_tuple<uint8_t, uint8_t, uint8_t>(0, 0, 0);
        cues[1].hotCueNumber = 2;
        cues[1].timeMs = 2000;
        cues[1].color = std::make_tuple<uint8_t, uint8_t, uint8_t>(255, 255, 255);
        cues[2].hotCueNumber = 3;
        cues[2].timeMs = 3000;  // no colour at all

        const auto decoded = AnlzCueCodec::decodeHotCues(AnlzCueCodec::encodeHotCues(cues));
        assert(decoded.size() == 3);
        assert(decoded[0].color.has_value() && "black is a colour, not the absence of one");
        assert((decoded[0].color == std::make_tuple<uint8_t, uint8_t, uint8_t>(0, 0, 0)));
        assert((decoded[1].color == std::make_tuple<uint8_t, uint8_t, uint8_t>(255, 255, 255)));
        assert(!decoded[2].color.has_value() && "and no colour is still no colour");
        std::cout << "case 9 (black and white round trip, and neither is 'no colour') OK\n";
    }

    // The ends of the ranges the fields hold: the last hot cue slot, and
    // a time far enough in to exercise the full 32 bits. 0xFFFFFFFF is
    // the codec's own "not a loop" sentinel, so a cue AT that time is
    // the one value where a time and a sentinel could be confused.
    {
        std::vector<RawHotCueEntry> cues(3);
        cues[0].hotCueNumber = 8;  // the last pad
        cues[0].timeMs = 0;        // and the first instant
        cues[1].hotCueNumber = 1;
        cues[1].timeMs = 0xFFFFFFFEu;
        cues[2].hotCueNumber = 2;
        cues[2].timeMs = 0xFFFFFFFFu;  // the sentinel's own value, as a time

        const auto decoded = AnlzCueCodec::decodeHotCues(AnlzCueCodec::encodeHotCues(cues));
        assert(decoded.size() == 3);
        assert(decoded[0].hotCueNumber == 8 && decoded[0].timeMs == 0);
        assert(!decoded[0].isLoop && "a cue at 0:00 is not a loop");
        assert(decoded[1].timeMs == 0xFFFFFFFEu);
        assert(decoded[2].timeMs == 0xFFFFFFFFu && !decoded[2].isLoop);
        std::cout << "case 10 (the last pad, the first instant, and a time equal to the loop sentinel) OK\n";
    }

    // Loops at the edges: one that ends where it starts, and one that
    // starts at zero. A zero-length loop is not something a DJ sets on
    // purpose, which is exactly why it must survive a rewrite rather
    // than being quietly turned into a plain cue.
    {
        std::vector<RawHotCueEntry> cues(2);
        cues[0].hotCueNumber = 1;
        cues[0].timeMs = 4000;
        cues[0].isLoop = true;
        cues[0].loopEndMs = 4000;  // zero length
        cues[1].hotCueNumber = 2;
        cues[1].timeMs = 0;
        cues[1].isLoop = true;
        cues[1].loopEndMs = 8000;

        const auto decoded = AnlzCueCodec::decodeHotCues(AnlzCueCodec::encodeHotCues(cues));
        assert(decoded.size() == 2);
        assert(decoded[0].isLoop && decoded[0].timeMs == 4000 && decoded[0].loopEndMs == 4000);
        assert(decoded[1].isLoop && decoded[1].timeMs == 0 && decoded[1].loopEndMs == 8000);
        std::cout << "case 11 (a zero-length loop and a loop from 0:00 both survive) OK\n";
    }

    // The shape a real save has: some entries carried over untouched
    // (rawBytes, with their comments and legacy colour ids) and one new
    // one beside them. Both the entry sizes and the section header have
    // to come out right when the two kinds are mixed, and every earlier
    // round-trip case used one kind or the other.
    {
        // The same real section case 1 decodes: one hot cue, no colour,
        // time 9875 ms, off "Another Brick In The Wall".
        const auto real =
            fromHex("50434f320000001400000040000000010001000050435032000000100000002c00000001010003e"
                    "8000026930000000000030132000000000000000000000000");
        auto carried = AnlzCueCodec::decodeHotCues(real);
        assert(!carried.empty() && "the real section must decode, or this case proves nothing");
        for (const auto &entry : carried) {
            assert(!entry.rawBytes.empty() && "a decoded entry keeps its bytes");
        }
        const std::size_t carriedCount = carried.size();

        RawHotCueEntry added;
        added.hotCueNumber = 7;
        added.timeMs = 99'000;
        added.color = std::make_tuple<uint8_t, uint8_t, uint8_t>(9, 8, 7);
        carried.push_back(added);

        const std::string mixed = AnlzCueCodec::encodeHotCues(carried);
        const auto decoded = AnlzCueCodec::decodeHotCues(mixed);
        assert(decoded.size() == carriedCount + 1);
        for (std::size_t i = 0; i < carriedCount; ++i) {
            assert(decoded[i].rawBytes == carried[i].rawBytes && "a carried entry comes back byte for byte");
        }
        assert(decoded[carriedCount].hotCueNumber == 7);
        assert(decoded[carriedCount].timeMs == 99'000);
        assert((decoded[carriedCount].color == std::make_tuple<uint8_t, uint8_t, uint8_t>(9, 8, 7)));
        std::cout << "case 12 (a new cue added beside real carried ones: sizes and header still agree) OK\n";
    }

    // A cue with a comment on it, and the comment is not ASCII.
    //
    // This codec writes no comments of its own (len_comment is 0 on
    // every fresh entry), so the only way one survives a save is the
    // rawBytes path: a rewrite that replaces the whole list must put
    // back, untouched, the comment on a cue it did not touch. Nothing
    // had tested that with a comment actually present -- the seven real
    // entries this was built against have none -- and a comment is
    // where a DJ's own words live, in UTF-16, which is where non-ASCII
    // turns up first.
    //
    // Built by hand rather than captured, because no real entry with a
    // comment has been captured yet. It is the documented layout: the
    // 44-byte fixed part, then len_comment bytes of UTF-16, and the
    // decoder's colour test reads (len_entry - len_comment) > 44, so a
    // comment long enough to push the entry past 44 bytes would be read
    // as a colour if that subtraction were ever dropped.
    {
        // "Cafe" with an acute on the e: 00 43 00 61 00 66 00 E9.
        const std::string comment = fromHex("00430061006600e9");
        std::string entry;
        entry += "PCP2";
        appendU32(entry, 16);                                        // len_header
        appendU32(entry, static_cast<uint32_t>(44 + comment.size()));  // len_entry
        appendU32(entry, 4);                                         // hot_cue: pad 4
        entry.push_back(1);                                          // cue point, not a loop
        entry += fromHex("0003e8");                                  // pad3, as in every real entry
        appendU32(entry, 61'000);                                    // time
        appendU32(entry, 0xFFFFFFFFu);                               // not a loop
        entry.push_back(0);                                          // color_id
        entry += fromHex("01002a");                                  // pad7: 0x01 then a counter
        entry += std::string(4, '\0');
        appendU32(entry, 0);                                         // loop numerator + denominator
        appendU32(entry, static_cast<uint32_t>(comment.size()));     // len_comment
        entry += comment;
        assert(entry.size() == 44 + comment.size());

        std::string section;
        section += "PCO2";
        appendU32(section, 20);
        appendU32(section, static_cast<uint32_t>(12 + entry.size()));
        appendU32(section, CueListTypeHot);
        appendU16(section, 1);  // num_cues
        appendU16(section, 0);  // padding
        section += entry;

        const auto decoded = AnlzCueCodec::decodeHotCues(section);
        assert(decoded.size() == 1);
        assert(decoded[0].hotCueNumber == 4);
        assert(decoded[0].timeMs == 61'000);
        assert(!decoded[0].color.has_value() && "the comment's bytes are not a colour");
        assert(decoded[0].rawBytes == entry && "the comment is kept, byte for byte");

        // And it comes back out the same way, which is the promise the
        // header makes: a save that rewrites the list does not flatten
        // the comment of a cue it never touched.
        const std::string rewritten = AnlzCueCodec::encodeHotCues(decoded);
        const auto again = AnlzCueCodec::decodeHotCues(rewritten);
        assert(again.size() == 1);
        assert(again[0].rawBytes == entry);
        assert(rewritten.find(comment) != std::string::npos && "the DJ's own words are still in the file");
        std::cout << "case 13 (a cue comment with non-ASCII in it survives a rewrite) OK\n";
    }

    // An entry that declares no length at all is refused, not repeated.
    // Nothing advanced `offset` by it, so the same 44 bytes came back
    // num_cues times -- up to 65535 copies of one hot cue out of a file
    // this parser never called malformed. The legacy PCOB codec beside
    // this one has always refused it; this one did not.
    {
        std::string entry;
        entry += "PCP2";
        appendU32(entry, 16);   // len_header
        appendU32(entry, 0);    // len_entry: the claim that matters
        appendU32(entry, 1);    // hot_cue: pad 1
        entry.push_back(1);
        entry += fromHex("0003e8");
        appendU32(entry, 1'000);
        appendU32(entry, 0xFFFFFFFFu);
        entry.push_back(0);
        entry += fromHex("01002a");
        entry += std::string(4, '\0');
        appendU32(entry, 0);
        appendU32(entry, 0);  // len_comment
        assert(entry.size() == 44);

        std::string section;
        section += "PCO2";
        appendU32(section, 20);
        appendU32(section, static_cast<uint32_t>(12 + entry.size()));
        appendU32(section, CueListTypeHot);
        appendU16(section, 500);  // num_cues: what a repeat would multiply by
        appendU16(section, 0);
        section += entry;

        bool threw = false;
        try {
            const auto decoded = AnlzCueCodec::decodeHotCues(section);
            std::cerr << "a zero-length entry decoded into " << decoded.size() << " cue(s)\n";
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw && "an entry shorter than one entry is a malformed section, not 500 cues");
        std::cout << "case 14 (a cue entry declaring no length is refused) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
