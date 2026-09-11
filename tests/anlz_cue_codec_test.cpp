#include <cassert>
#include <stdexcept>
#include <iostream>

#include "infrastructure/rekordbox/anlz_cue_codec.hpp"

using namespace seabass::infrastructure::rekordbox;

namespace
{

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

    std::cout << "all cases passed\n";
    return 0;
}
