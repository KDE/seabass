#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "application/ports/cancellation_token.hpp"
#include "infrastructure/benchmark/stick_performance_probe.hpp"
#include "infrastructure/benchmark/stick_surface_check.hpp"
#include "infrastructure/benchmark/stick_write_probe.hpp"
#include "infrastructure/local/stick_performance_history.hpp"

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
        assert(m.catalogBytes == 64 * 1024);
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
        assert(m.catalogBytes == 0);
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

    // Case 6: asked to keep its files, the write test leaves them for the
    // read probe (a blank stick has nothing else to read), and
    // removeScratch() takes them away afterwards.
    {
        fs::path stick = root / "blank";
        fs::create_directories(stick);
        WriteProbeOptions options;
        options.streamingFiles = 2;
        options.streamingBytesPerFile = 256 * 1024;
        options.smallFiles = 8;
        options.inPlaceUpdates = 4;
        options.inPlaceFileBytes = 64 * 1024;
        options.minimumFreeBytes = 1;
        ScratchFiles files;
        auto w = StickWriteProbe::run(stick.string(), seabass::application::CancellationToken::none(), options, &files);
        assert(w.smallFilesWritten == 8);
        assert(files.streamFiles.size() == 2);
        assert(files.smallFiles.size() == 8);
        assert(fs::exists(files.folder));
        for (const auto &f : files.streamFiles) {
            assert(fs::file_size(f) == 256 * 1024);
        }
        ProbeOptions readOptions;
        readOptions.randomReads = 20;
        auto m = StickPerformanceProbe::run(files.streamFiles, files.smallFiles, {},
                                            seabass::application::CancellationToken::none(), readOptions);
        assert(m.streamingBytesPerSecond > 0.0);
        assert(m.randomReads == 20);
        assert(m.smallFilesRead == 8);
        StickWriteProbe::removeScratch(stick.string());
        assert(!fs::exists(stick / StickWriteProbe::kScratchFolderName));
        std::cout << "case 6 (kept scratch files are readable, then removed) OK\n";
    }

    // Case 7: outliers are counted against the median, and only once
    // there are enough values for a median to mean something.
    {
        assert(storageprobe::outlierCount({1.0, 1.1, 0.9, 1.0, 1.2, 9.0, 1.0, 40.0}) == 2);
        assert(storageprobe::outlierCount({1.0, 1.0, 1.0, 1.0}) == 0);
        assert(storageprobe::outlierCount({1.0, 100.0, 100.0}) == 0);
        assert(storageprobe::outlierCount({}) == 0);
        std::cout << "case 7 (outlier count) OK\n";
    }

    // Case 8: the surface check reads every file, counts what it read,
    // reports progress, and flags a truncated file as unreadable.
    {
        fs::path stick = root / "surface";
        writeFile(stick / "Contents" / "a.mp3", 2 * 1024 * 1024);
        writeFile(stick / "Contents" / "b.mp3", 3 * 1024 * 1024);
        writeFile(stick / "PIONEER" / "USBANLZ" / "x" / "ANLZ0000.DAT", 8 * 1024);
        writeFile(stick / ".hidden" / "note.txt", 100);
        int progressCalls = 0;
        std::uint64_t lastTotal = 0;
        auto check = StickSurfaceCheck::run(stick.string(), [&](std::uint64_t, std::uint64_t total, std::uint64_t, std::uint64_t) {
            ++progressCalls;
            lastTotal = total;
        });
        assert(check.filesRead == 4);
        assert(check.bytesRead == 5 * 1024 * 1024 + 8 * 1024 + 100);
        assert(lastTotal == check.bytesRead);
        assert(progressCalls == 4);
        assert(check.medianBytesPerSecond > 0.0);
        assert(check.unreadable.empty());
        assert(check.unopenable.empty());
        assert(check.seconds >= 0.0);

#if !defined(_WIN32)
        // A file that cannot be opened is skipped and reported apart from
        // media failures: nothing was read, so nothing is known about
        // the flash under it. (Root can open anything; skip there.)
        if (::geteuid() != 0) {
            fs::path locked = stick / "Contents" / "locked.mp3";
            writeFile(locked, 4096);
            fs::permissions(locked, fs::perms::none);
            auto again = StickSurfaceCheck::run(stick.string());
            fs::permissions(locked, fs::perms::owner_all);
            assert(again.unopenable.size() == 1 && again.unopenable[0] == locked.string());
            assert(again.unreadable.empty());
            auto wear = seabass::domain::assessWear(again, seabass::domain::StickPerformanceMeasurement{});
            assert(wear.state == seabass::domain::WearState::Healthy);
            assert(wear.summary.find("could not be opened") != std::string::npos);
        }
#endif

        // A file whose directory size claims more than it delivers is
        // what a failing read looks like to a backup; simulated with a
        // file that shrinks between the walk and the read. Achieved by
        // a symlink to a shorter file under the name of a longer one is
        // not portable, so the check's own truncation rule is exercised
        // through the domain instead (see stick_performance_test).
        bool cancelledThrown = false;
        seabass::application::CancellationToken cancel;
        cancel.cancel();
        try {
            StickSurfaceCheck::run(stick.string(), {}, cancel);
        } catch (const seabass::application::OperationCancelled &) {
            cancelledThrown = true;
        }
        assert(cancelledThrown);
        std::cout << "case 8 (surface check reads everything, reports, cancels) OK\n";
    }

    // Case 9: the local history round-trips, keeps sticks apart, caps at
    // twenty per stick, and takes a wear state onto the newest record.
    {
        using seabass::infrastructure::local::StickPerformanceHistory;
        using seabass::infrastructure::local::StickPerformanceRecord;
        fs::path file = root / "history" / "stick-performance-history.tsv";
        StickPerformanceHistory history(file);
        assert(history.forStick("A").empty());
        for (int i = 0; i < 25; ++i) {
            StickPerformanceRecord r;
            r.measuredAtUtc = "2026-09-" + std::string(i < 9 ? "0" : "") + std::to_string(i + 1) + "T10:00:00Z";
            r.stickIdentifier = "A";
            r.stickLabel = "Tab\tin label";
            r.score = 50 + i;
            r.streamingBytesPerSecond = 40e6;
            r.randomReadMedianMs = 1.1;
            r.smallFileMedianMs = 1.2;
            r.outliers = i % 3;
            r.usbSpeedMbps = 480.0;
            history.append(r);
        }
        StickPerformanceRecord other;
        other.measuredAtUtc = "2026-09-11T11:00:00Z";
        other.stickIdentifier = "B";
        other.stickLabel = "RV2";
        other.score = 32;
        history.append(other);

        auto a = history.forStick("A");
        assert(a.size() == StickPerformanceHistory::kKeepPerStick);
        assert(a.front().score == 55);  // the five oldest dropped
        assert(a.back().score == 74);
        assert(a.back().stickLabel == "Tab in label");
        assert(a.back().randomReadMedianMs > 1.09 && a.back().randomReadMedianMs < 1.11);
        assert(a.back().wearState.empty());
        assert(a.back().usbSpeedMbps == 480.0);
        assert(history.forStick("B").size() == 1);

        history.setLatestWearState("A", "watch");
        StickPerformanceHistory reopened(file);
        auto again = reopened.forStick("A");
        assert(again.back().wearState == "watch");
        assert(again[again.size() - 2].wearState.empty());
        assert(reopened.forStick("B").front().wearState.empty());
        std::cout << "case 9 (local history round trip, cap, wear state) OK\n";
    }

    fs::remove_all(root);
    std::cout << "All stick_performance_probe tests passed.\n";
    return 0;
}
