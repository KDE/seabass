#include "gui/stick_catalogs.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "gui/library_catalog_cache.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/stick_layout.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

StickCatalogRead readAllStickCatalogs(const std::string &libraryPath, application::ProgressReporter &progress,
                                        const application::CancellationToken &cancel)
{
    StickCatalogRead result;
    if (libraryPath.empty()) {
        return result;
    }

    const fs::path root = fs::path(libraryPath).parent_path();
    const fs::path pioneerRoot = root / "PIONEER";

    auto read = [&](const char *format, const std::string &path,
                    std::optional<std::vector<domain::Track>> &into) {
        try {
            into = LibraryCatalogCache::instance().tracksFor(format, path, progress, cancel);
        } catch (const application::OperationCancelled &) {
            throw;  // a cancelled scan is not a failed catalog
        } catch (const std::exception &) {
            result.failed.emplace_back(format);
        }
    };

    std::error_code ec;
    if (fs::exists(pioneerRoot / "rekordbox" / "export.pdb", ec)) {
        read("rekordbox", pioneerRoot.string(), result.catalogs.rekordbox);
    }
    if (fs::exists(infrastructure::engine::engineMainDatabasePath(root), ec)) {
        read("engine", infrastructure::engine::engineLibraryPath(root).string(), result.catalogs.engine);
    }
    if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneerRoot.string())) {
        read("onelibrary", pioneerRoot.string(), result.catalogs.oneLibrary);
    }
    return result;
}

std::int64_t catalogsLastModified(const std::string &libraryPath)
{
    if (libraryPath.empty()) {
        return 0;
    }
    const fs::path root = fs::path(libraryPath).parent_path();
    const fs::path pioneerRoot = root / "PIONEER";

    // The same three catalogs readAllStickCatalogs() consults, named by
    // the file each one actually stores cues in. exportExt.pdb is
    // deliberately not among them: it holds the extended tags rekordbox
    // writes alongside export.pdb, and nothing this rule weighs.
    const fs::path candidates[] = {
        pioneerRoot / "rekordbox" / "export.pdb",
        infrastructure::engine::engineMainDatabasePath(root),
        pioneerRoot / "rekordbox" / "exportLibrary.db",
    };

    std::int64_t newest = 0;
    for (const auto &path : candidates) {
        std::error_code ec;
        const auto written = fs::last_write_time(path, ec);
        if (ec) {
            continue;  // absent, or unreadable: not a date, so not an answer
        }
        // file_clock to system_clock. clock_cast is the correct
        // conversion and libstdc++ has not had it for file_clock for
        // long enough to rely on across the platforms Seabass builds
        // for; file_clock::to_sys is the portable spelling that has
        // been there since C++20 landed.
        const auto asSystemTime = std::chrono::file_clock::to_sys(written);
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(asSystemTime.time_since_epoch()).count();
        newest = std::max<std::int64_t>(newest, static_cast<std::int64_t>(seconds));
    }
    return newest;
}

}  // namespace seabass::gui
