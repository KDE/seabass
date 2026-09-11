#include "gui/stick_catalogs.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "gui/library_catalog_cache.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/stick_backup/library_catalog_mtime.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"
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

    // stick_backup's helper does the hard part, and the hard part is
    // Engine's write-ahead log.
    //
    // This function was originally a second, hand-rolled list of catalog
    // files that stat'ed m.db and stopped there. SQLite in WAL mode
    // writes commits to the -wal sidecar and only folds them back on a
    // checkpoint, so a track re-cued in Engine last night can leave
    // m.db's own mtime weeks old. The merge rule would then read that
    // stale date, decide the stick was older than the store, and both
    // refuse to back the new cues up and offer to overwrite them on a
    // restore -- the exact failure the rule exists to prevent, caused by
    // the date it rests on.
    std::int64_t newest = infrastructure::stick_backup::libraryCatalogModifiedAt(root);

    // Plus Device Library Plus, which that helper does not consult
    // because a stick backup does not need it and this does: it is a
    // third catalog a DJ's cues can live in, so a cue edit that lands
    // only there still has to date the stick.
    const fs::path oneLibrary = root / "PIONEER" / "rekordbox" / "exportLibrary.db";
    for (const fs::path &candidate : {oneLibrary, fs::path(oneLibrary.string() + "-wal")}) {
        std::error_code ec;
        const fs::file_time_type written = fs::last_write_time(candidate, ec);
        if (ec) {
            continue;  // absent, or unreadable: not a date, so not an answer
        }
        newest = std::max(newest, infrastructure::stick_backup::toUnixSeconds(written));
    }
    return newest;
}

}  // namespace seabass::gui
