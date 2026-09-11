// Runs the two performance probes against a real stick, the way the USB
// Stick Performance page does, and prints what the page would show.
// Skips itself unless SEABASS_LIVE_STICK names the stick's mount point;
// the write probe additionally needs SEABASS_LIVE_WRITE=1, since it
// writes (and removes) about 22 MiB on the stick. Under ctest neither is
// set and this passes without touching anything.
//
// SEABASS_LIVE_WEAR=1 adds the surface check, which reads every file on
// the stick once (minutes on a big stick).
//
//   SEABASS_LIVE_STICK=/media/you/STICK SEABASS_LIVE_WRITE=1 SEABASS_LIVE_WEAR=1 ./stick_performance_live_test

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "domain/stick_performance.hpp"
#include "infrastructure/benchmark/stick_performance_probe.hpp"
#include "infrastructure/benchmark/stick_surface_check.hpp"
#include "infrastructure/benchmark/stick_write_probe.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

bool isAudio(const fs::path &p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".m4a" ||
           ext == ".ogg";
}

// Every regular file under dir that `accept` likes, skipping hidden
// folders (this project's own backups live in .seabass-backups).
template <typename Accept>
std::vector<std::string> collect(const fs::path &dir, Accept accept)
{
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return out;
    }
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && it->path().filename().string().rfind('.', 0) == 0) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && accept(it->path())) {
            out.push_back(it->path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> spread(const std::vector<std::string> &files, std::size_t count)
{
    if (files.size() <= count) {
        return files;
    }
    std::vector<std::string> out;
    std::size_t step = files.size() / count;
    for (std::size_t i = 0; i < files.size() && out.size() < count; i += step) {
        out.push_back(files[i]);
    }
    return out;
}

std::string verdict(domain::Verdict v)
{
    return domain::verdictLabel(v);
}

}  // namespace

int main()
{
    const char *stickEnv = std::getenv("SEABASS_LIVE_STICK");
    if (stickEnv == nullptr || *stickEnv == '\0') {
        std::cout << "SEABASS_LIVE_STICK not set; skipping the live stick performance test\n";
        return 0;
    }
    fs::path root(stickEnv);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        std::cerr << root << " is not a directory\n";
        return 1;
    }

    std::vector<std::string> databaseFiles;
    for (const auto &candidate : {root / "PIONEER" / "rekordbox" / "export.pdb", root / "Engine Library" / "Database2" / "m.db"}) {
        if (fs::exists(candidate, ec)) {
            databaseFiles.push_back(candidate.string());
        }
    }
    auto allAudio = collect(root, isAudio);
    auto audio = spread(allAudio, 20);
    auto analysis = collect(root / "PIONEER" / "USBANLZ", [](const fs::path &p) { return p.filename() == "ANLZ0000.DAT"; });
    if (analysis.empty()) {
        analysis = collect(root / "Engine Library" / "Database2" / "OverviewData", [](const fs::path &) { return true; });
    }
    std::shuffle(analysis.begin(), analysis.end(), std::mt19937(0x5EABA55u));
    if (analysis.size() > 150) {
        analysis.resize(150);
    }
    std::cout << "stick: " << root << "\n  audio files: " << allAudio.size() << " (sampling " << audio.size() << ")"
              << "\n  analysis files sampled: " << analysis.size() << "\n  database files: " << databaseFiles.size() << "\n";
    assert(!audio.empty() && "no audio on this stick; nothing to measure");

    auto m = infrastructure::benchmark::StickPerformanceProbe::run(audio, analysis, databaseFiles);
    auto s = domain::scoreDjWorkload(m);
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\nread probe\n"
              << "  streaming read        " << m.streamingBytesPerSecond / 1e6 << " MB/s\n"
              << "  small random read     " << m.randomReadMedianMs << " ms median, " << m.randomReadP95Ms << " ms p95 ("
              << m.randomReads << " reads)\n"
              << "  small file open+read  " << m.smallFileOpensPerSecond << " files/s, " << m.smallFileMedianMs
              << " ms median (" << m.smallFilesRead << " files)\n"
              << "  slow-tail outliers    " << m.randomReadOutliers << " random reads, " << m.smallFileOutliers
              << " small files over " << storageprobe::kOutlierFactor << "x the median\n"
              << "  database bytes        " << m.catalogBytes << "\n";
    std::cout << "\nDJ Workload Score " << s.score << " (" << domain::speedClassLabel(s.speedClass) << ")\n"
              << "  browsing " << s.browseScore << " " << verdict(s.browseVerdict) << ", track loads " << s.trackLoadScore
              << " " << verdict(s.trackLoadVerdict) << ", plugging in " << s.mountScore << " " << verdict(s.mountVerdict)
              << ", streaming " << verdict(s.streamingVerdict) << "\n  " << domain::describeSetWait(s) << "\n";
    for (const auto &row : domain::advisePlayers(m, s)) {
        std::cout << "  " << row.group << ": " << verdict(row.verdict) << "\n    " << row.summary << "\n";
    }
    assert(m.streamingBytesPerSecond > 0.0);
    assert(m.randomReads > 0);

    const char *wearEnv = std::getenv("SEABASS_LIVE_WEAR");
    if (wearEnv != nullptr && std::string(wearEnv) == "1") {
        std::uint64_t lastFiles = 0;
        auto check = infrastructure::benchmark::StickSurfaceCheck::run(root.string(),
            [&](std::uint64_t bytesDone, std::uint64_t bytesTotal, std::uint64_t filesDone, std::uint64_t filesTotal) {
                if (filesDone - lastFiles >= 500 || filesDone == filesTotal) {
                    lastFiles = filesDone;
                    std::cout << "  surface " << filesDone << "/" << filesTotal << " files, "
                              << bytesDone / (1024.0 * 1024.0) << " of " << bytesTotal / (1024.0 * 1024.0) << " MiB\r"
                              << std::flush;
                }
            });
        auto wear = domain::assessWear(check, m);
        std::cout << "\nwear check\n"
                  << "  read " << check.filesRead << " files, " << check.bytesRead / (1024.0 * 1024.0) << " MiB in "
                  << check.seconds << " s, median " << check.medianBytesPerSecond / 1e6 << " MB/s per file\n"
                  << "  unreadable " << check.unreadable.size() << ", slow " << check.slow.size() << "\n"
                  << "  " << wear.label << ": " << wear.summary << "\n";
        for (const auto &f : check.slow) {
            std::cout << "    slow " << f.bytesPerSecond / 1e6 << " MB/s  " << f.path << "\n";
        }
        for (const auto &f : check.unreadable) {
            std::cout << "    unreadable " << f << "\n";
        }
        assert(check.filesRead > 0);
    } else {
        std::cout << "\nSEABASS_LIVE_WEAR is not 1; skipping the surface check\n";
    }

    const char *writeEnv = std::getenv("SEABASS_LIVE_WRITE");
    if (writeEnv == nullptr || std::string(writeEnv) != "1") {
        std::cout << "\nSEABASS_LIVE_WRITE is not 1; skipping the write probe\n";
        return 0;
    }
    auto freeBefore = fs::space(root, ec).available;
    auto w = infrastructure::benchmark::StickWriteProbe::run(root.string());
    auto freeAfter = fs::space(root, ec).available;
    auto e = domain::estimateWriteWorkloads(w);
    std::cout << "\nwrite probe (wrote " << w.bytesWritten / (1024.0 * 1024.0) << " MiB of throwaway files)\n"
              << "  streaming write       " << w.streamingWriteBytesPerSecond / 1e6 << " MB/s\n"
              << "  small file write      " << w.smallFileWriteMedianMs << " ms median, " << w.smallFileWritesPerSecond
              << " files/s (" << w.smallFilesWritten << " files)\n"
              << "  in-place update       " << w.inPlaceUpdateMedianMs << " ms median (" << w.inPlaceUpdates << " updates)\n"
              << "  cue save              " << e.cueSaveSeconds << " s " << verdict(e.cueSaveVerdict) << "\n"
              << "  export, 100 tracks    " << e.exportHundredTracksSeconds << " s (" << e.exportTrackSeconds
              << " s per track) " << verdict(e.exportVerdict) << "\n";
    assert(!fs::exists(root / infrastructure::benchmark::StickWriteProbe::kScratchFolderName));
    // The scratch folder is gone, so free space is back where it was
    // (FAT reports it in clusters; allow one MiB of slack either way).
    assert(freeAfter + (1u << 20) >= freeBefore && freeBefore + (1u << 20) >= freeAfter);
    std::cout << "  scratch folder removed, free space unchanged\n";
    std::cout << "all cases passed\n";
    return 0;
}
