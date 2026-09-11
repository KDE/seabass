#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "application/ports/cancellation_token.hpp"
#include "infrastructure/benchmark/stick_performance_probe.hpp"
#include "infrastructure/benchmark/stick_write_probe.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::benchmark;
namespace fs = std::filesystem;

namespace
{

void writeFile(const fs::path &path, std::size_t bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::string chunk(bytes, 'x');
    out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_stick_performance_probe_test";
    fs::remove_all(root);
    fs::create_directories(root);

    // Case 1: all three read measurements come back from real files, with
    // the counts they were given.
    {
        std::vector<std::string> audio, small;
        for (int i = 0; i < 3; ++i) {
            fs::path p = root / ("audio-" + std::to_string(i) + ".mp3");
            writeFile(p, 256 * 1024);
            audio.push_back(p.string());
        }
        for (int i = 0; i < 20; ++i) {
            fs::path p = root / "anlz" / std::to_string(i) / "ANLZ0000.DAT";
            writeFile(p, 8 * 1024);
            small.push_back(p.string());
        }
        fs::path db = root / "export.pdb";
        writeFile(db, 64 * 1024);

        ProbeOptions options;
        options.randomReads = 40;
        auto m = StickPerformanceProbe::run(audio, small, {db.string()}, seabass::application::CancellationToken::none(),
                                            options);
        assert(m.streamingBytesPerSecond > 0.0);
        assert(m.randomReads == 40);
        assert(m.randomReadMedianMs > 0.0);
        assert(m.randomReadP95Ms >= m.randomReadMedianMs);
        assert(m.smallFilesRead == 20);
        assert(m.smallFileMedianMs > 0.0);
        assert(m.smallFileOpensPerSecond > 0.0);
        assert(m.databaseBytes == 64 * 1024);
        std::cout << "case 1 (real files measured on all three dimensions) OK\n";
    }

    // Case 2: missing files are skipped, never fatal; nothing at all
    // measures as zero, never as an error.
    {
        fs::path audio = root / "present.mp3";
        writeFile(audio, 64 * 1024);
        auto m = StickPerformanceProbe::run({(root / "missing.mp3").string(), audio.string()},
                                            {(root / "missing.DAT").string()}, {(root / "missing.pdb").string()});
        assert(m.streamingBytesPerSecond > 0.0);
        assert(m.smallFilesRead == 0 && m.smallFileOpensPerSecond == 0.0);
        assert(m.databaseBytes == 0);
        auto none = StickPerformanceProbe::run({}, {}, {});
        assert(none.streamingBytesPerSecond == 0.0 && none.randomReads == 0 && none.smallFilesRead == 0);
        std::cout << "case 2 (missing files skipped, empty input is zero) OK\n";
    }

    // Case 3: a file too small for random reads is left out of that
    // measurement but still streamed.
    {
        fs::path tiny = root / "tiny.mp3";
        writeFile(tiny, 4096);
        auto m = StickPerformanceProbe::run({tiny.string()}, {}, {});
        assert(m.streamingBytesPerSecond > 0.0);
        assert(m.randomReads == 0);
        std::cout << "case 3 (tiny file streams, no random reads) OK\n";
    }

    // Case 4: the write test writes into its own hidden folder and leaves
    // nothing behind, and reports what it wrote.
    {
        fs::path stick = root / "stick";
        fs::create_directories(stick / "PIONEER");
        WriteProbeOptions options;
        options.streamingFiles = 1;
        options.streamingBytesPerFile = 2 * 1024 * 1024;
        options.smallFiles = 10;
        options.inPlaceUpdates = 10;
        options.inPlaceFileBytes = 256 * 1024;
        options.minimumFreeBytes = 1;
        auto m = StickWriteProbe::run(stick.string(), seabass::application::CancellationToken::none(), options);
        assert(m.streamingWriteBytesPerSecond > 0.0);
        assert(m.smallFilesWritten == 10);
        assert(m.smallFileWriteMedianMs > 0.0 && m.smallFileWritesPerSecond > 0.0);
        assert(m.inPlaceUpdates == 10);
        assert(m.inPlaceUpdateMedianMs > 0.0);
        assert(m.bytesWritten >= 2 * 1024 * 1024 + 10 * 16 * 1024 + 256 * 1024 + 10 * 4096);
        assert(!fs::exists(stick / StickWriteProbe::kScratchFolderName));
        assert(fs::exists(stick / "PIONEER"));
        std::cout << "case 4 (write test measures and cleans up) OK\n";
    }

    // Case 5: a cancelled write test unwinds and still cleans up; a
    // missing root refuses.
    {
        fs::path stick = root / "stick2";
        fs::create_directories(stick);
        seabass::application::CancellationToken cancel;
        cancel.cancel();
        bool thrown = false;
        try {
            StickWriteProbe::run(stick.string(), cancel);
        } catch (const seabass::application::OperationCancelled &) {
            thrown = true;
        }
        assert(thrown);
        assert(!fs::exists(stick / StickWriteProbe::kScratchFolderName));

        bool refused = false;
        try {
            StickWriteProbe::run((root / "does-not-exist").string());
        } catch (const std::runtime_error &) {
            refused = true;
        }
        assert(refused);
        std::cout << "case 5 (cancelled write test cleans up; missing root refused) OK\n";
    }

    fs::remove_all(root);
    std::cout << "All stick_performance_probe tests passed.\n";
    return 0;
}
