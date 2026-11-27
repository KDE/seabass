// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/kept_analysis_save.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

#include "application/path_key.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

namespace
{

constexpr const char *Label = "track-reanalysis";

// A missing ledger is an empty one; one that cannot be believed is none.
struct SaveLedger
{
    infrastructure::local::AnalysisLedger ledger;
    bool believed = false;
    bool incomplete = false;
};

// The kept files a save's store touched, for the names Browse counts by.
struct TouchedKeptFiles
{
    std::set<std::string> files;
};

}  // namespace

std::string keptRekordboxAnalysisFileOf(const std::string &stickRoot, const std::string &ledgerKey)
{
    return ledgerKey.empty() || stickRoot.empty()
               ? std::string()
               : pathToUtf8(infrastructure::local::keptRekordboxAnalysisFile(pathFromUtf8(stickRoot), ledgerKey));
}

std::string keptRekordboxRecordFileOf(const std::string &stickRoot, const std::string &ledgerKey)
{
    return ledgerKey.empty() || stickRoot.empty()
               ? std::string()
               : pathToUtf8(infrastructure::local::keptRekordboxRecordFile(pathFromUtf8(stickRoot), ledgerKey));
}

infrastructure::local::StickKeptRekordboxAnalysisStore &saveKeptRekordboxStore(SaveContext &ctx,
                                                                              const std::string &stickRoot)
{
    const std::string key = "kept-rekordbox-analysis:" + application::normalizedPathKey(stickRoot);
    return ctx.shared<infrastructure::local::StickKeptRekordboxAnalysisStore>(key, [&]() {
        auto made = std::make_unique<infrastructure::local::StickKeptRekordboxAnalysisStore>(pathFromUtf8(stickRoot));
        bool created = false;
        auto &touched = ctx.sharedForWholeSave<TouchedKeptFiles>(key + ":touched", [&created]() {
            created = true;
            return std::make_unique<TouchedKeptFiles>();
        });
        if (created) {
            // After the save, landed or taken back: each file as it is on
            // the stick then.
            ctx.onAfterCommit([&ctx, key, stickRoot](const SaveContext::AfterCommit &) {
                if (const auto *files = ctx.sharedForWholeSaveIfPresent<TouchedKeptFiles>(key + ":touched")) {
                    noteKeptAnalysisFilesChanged(pathFromUtf8(stickRoot), files->files);
                }
            });
        }
        made->setBeforeChange([&ctx, &touched](const std::string &file) {
            BackupTarget target;
            target.file = file;
            target.label = Label;
            target.removeOnRestoreIfAbsent = true;
            ctx.backupAllNow({target});
            ctx.protectForThisChange(file);
            touched.files.insert(file);
        });
        return made;
    });
}

const infrastructure::local::AnalysisLedger *saveAnalysisLedger(SaveContext &ctx, const std::string &stickRoot)
{
    const auto &read = ctx.sharedForWholeSave<SaveLedger>(
        "kept-rekordbox-analysis:ledger:" + application::normalizedPathKey(stickRoot), [&]() {
            auto made = std::make_unique<SaveLedger>();
            std::string error;
            infrastructure::local::AnalysisLedgerFault fault = infrastructure::local::AnalysisLedgerFault::None;
            std::string incompleteSince;
            made->ledger =
                infrastructure::local::readAnalysisLedger(pathFromUtf8(stickRoot), &error, &fault, &incompleteSince);
            made->believed = fault == infrastructure::local::AnalysisLedgerFault::None;
            made->incomplete = !incompleteSince.empty();
            return made;
        });
    return read.believed ? &read.ledger : nullptr;
}

bool ledgerIncomplete(SaveContext &ctx, const std::string &stickRoot)
{
    saveAnalysisLedger(ctx, stickRoot);
    const auto *read = ctx.sharedForWholeSaveIfPresent<SaveLedger>("kept-rekordbox-analysis:ledger:"
                                                                   + application::normalizedPathKey(stickRoot));
    return read != nullptr && read->believed && read->incomplete;
}

namespace
{

// Seabass's rekordbox grids as the stick records them, read once per save:
// the ledger's at once, the kept entries' heads only when asked.
struct SeabassGrids
{
    std::set<std::string> ledger;
    std::optional<std::set<std::string>> kept;
};

SeabassGrids &saveSeabassGrids(SaveContext &ctx, const std::string &stickRoot)
{
    return ctx.sharedForWholeSave<SeabassGrids>(
        "kept-rekordbox-analysis:seabass-grids:" + application::normalizedPathKey(stickRoot), [&]() {
            auto made = std::make_unique<SeabassGrids>();
            if (const auto *ledger = saveAnalysisLedger(ctx, stickRoot)) {
                for (const auto &[key, entry] : *ledger) {
                    if (!entry.rekordboxGrid.empty()) {
                        made->ledger.insert(entry.rekordboxGrid);
                    }
                }
            }
            return made;
        });
}

std::function<std::optional<std::uint64_t>(const std::string &)> &bytesAvailableHook()
{
    static std::function<std::optional<std::uint64_t>(const std::string &)> hook;
    return hook;
}

}  // namespace

bool ledgerBelieved(SaveContext &ctx, const std::string &stickRoot)
{
    return saveAnalysisLedger(ctx, stickRoot) != nullptr;
}

bool isSeabassRekordboxGrid(SaveContext &ctx, const std::string &stickRoot, const std::string &grid,
                            const infrastructure::local::KeptRekordboxRecord *own)
{
    if (grid.empty()) {
        return false;
    }
    if (own && std::find(own->seabassGrids.begin(), own->seabassGrids.end(), grid) != own->seabassGrids.end()) {
        return true;
    }
    SeabassGrids &grids = saveSeabassGrids(ctx, stickRoot);
    if (grids.ledger.count(grid) > 0) {
        return true;
    }
    if (!grids.kept) {
        std::string error;
        const auto heads = infrastructure::local::keptRekordboxSeabassGrids(pathFromUtf8(stickRoot), &error);
        grids.kept = std::set<std::string>(heads.begin(), heads.end());
    }
    return grids.kept->count(grid) > 0;
}

KeepOriginal keepOriginalDecision(const std::string &gridNow, bool gridIsSeabass, bool ledgerBelieved,
                                  const infrastructure::local::KeptRekordboxAnalysis *kept,
                                  const infrastructure::local::KeptRekordboxRecord *record)
{
    // A ledger that cannot be believed (damaged, unreadable) is not one
    // that knows nothing: the grid may be Seabass's own, and keeping it
    // as rekordbox's original would be a lie Restore later tells.
    if (!kept) {
        return gridIsSeabass ? KeepOriginal::Nothing : ledgerBelieved ? KeepOriginal::KeepNow : KeepOriginal::CannotTell;
    }
    // The kept entry and its record are the evidence for their track, the
    // ledger none: without the record nothing tells Seabass's grids.
    if (!record) {
        return KeepOriginal::CannotTell;
    }
    // A grid that cannot be told (none in the file) is no evidence of a
    // newer analysis: the kept one stays.
    if (gridNow.empty() || gridNow == kept->originalGrid || gridIsSeabass) {
        return KeepOriginal::KeptStays;
    }
    return KeepOriginal::ChangedOutside;
}

std::string keptEngineOriginalFingerprint(const application::KeptEngineAnalysis &entry)
{
    const auto value = [](const application::EngineSqlValue &v) {
        std::string real(sizeof v.real, '\0');
        std::memcpy(real.data(), &v.real, sizeof v.real);
        return std::to_string(static_cast<int>(v.type)) + ":" + std::to_string(v.integer) + ":" + real + ":"
               + std::to_string(v.bytes.size()) + ":" + v.bytes + "\n";
    };
    const application::EngineAnalysisColumns &c = entry.original;
    std::string text = "track:" + std::to_string(entry.trackId) + "\n";
    for (const auto *v : {&c.isAnalyzed, &c.isBeatGridLocked, &c.bpm, &c.bpmAnalyzed, &c.key, &c.trackData,
                          &c.overviewWaveFormData, &c.beatData}) {
        text += value(*v);
    }
    text += c.overviewFile ? "file:" + std::to_string(c.overviewFile->size()) + ":" + *c.overviewFile : "no file";
    return infrastructure::hashing::toHex(infrastructure::hashing::Sha256::of(std::string_view(text)));
}

std::vector<infrastructure::local::KeptCueMove> withCueMoves(const std::vector<infrastructure::local::KeptCueMove> &kept,
                                                             const std::vector<infrastructure::local::KeptCueMove> &made)
{
    std::vector<infrastructure::local::KeptCueMove> merged = kept;
    for (const auto &move : made) {
        const auto earlier = std::find_if(merged.begin(), merged.end(), [&](const auto &e) {
            return e.library == move.library && e.hot == move.hot && e.pad == move.pad && e.loop == move.loop
                   && std::abs(e.newMs - move.oldMs) < 1.0 && (!e.loop || std::abs(e.newEndMs - move.oldEndMs) < 1.0);
        });
        if (earlier != merged.end()) {
            earlier->newMs = move.newMs;
            earlier->newEndMs = move.newEndMs;
        } else {
            merged.push_back(move);
        }
    }
    return merged;
}

std::optional<std::uint64_t> stickBytesAvailable(const std::string &stickRoot)
{
    if (bytesAvailableHook()) {
        return bytesAvailableHook()(stickRoot);
    }
    std::error_code ec;
    const std::filesystem::space_info info = std::filesystem::space(pathFromUtf8(stickRoot), ec);
    return ec ? std::nullopt : std::optional<std::uint64_t>(info.available);
}

void setStickBytesAvailableForTesting(
    std::function<std::optional<std::uint64_t>(const std::string &stickRoot)> measure)
{
    bytesAvailableHook() = std::move(measure);
}

namespace
{

namespace fs = std::filesystem;

// What the playlist menu counts by, per stick (by its identity, not where
// it is mounted): the kept folders' names, and for each playlist the
// names its tracks' kept entries would have. `version` grows with every
// change noted, so a listing that started before one is not stored.
struct StickNames
{
    fs::path root;
    std::shared_ptr<const KeptAnalysisNames> names;
    std::uint64_t version = 0;
    std::map<std::string, std::vector<std::string>> playlists;
};

struct NameCache
{
    std::mutex mutex;
    std::map<std::string, StickNames> byStick;
};

NameCache &nameCache()
{
    static NameCache cache;
    return cache;
}

std::set<std::string> fileNamesIn(const fs::path &dir)
{
    std::set<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        names.insert(pathToUtf8(it->path().filename()));
    }
    return names;
}

std::string rootKey(const fs::path &root)
{
    return application::normalizedPathKey(pathToUtf8(root));
}

std::shared_ptr<const KeptAnalysisNames> listNames(const fs::path &root)
{
    auto names = std::make_shared<KeptAnalysisNames>();
    names->rekordbox = fileNamesIn(infrastructure::paths::stickKeptRekordboxAnalysisDir(root));
    names->engine = fileNamesIn(infrastructure::paths::stickKeptEngineAnalysisDir(root));
    return names;
}

}  // namespace

std::shared_ptr<const KeptAnalysisNames> relistKeptAnalysisNames(const std::string &stickId, const fs::path &root)
{
    NameCache &cache = nameCache();
    std::uint64_t version = 0;
    {
        std::lock_guard lock(cache.mutex);
        StickNames &stick = cache.byStick[stickId];
        if (rootKey(stick.root) != rootKey(root)) {
            stick = StickNames{root, nullptr, stick.version + 1, {}};
        }
        version = stick.version;
    }
    auto names = listNames(root);
    std::lock_guard lock(cache.mutex);
    StickNames &stick = cache.byStick[stickId];
    if (stick.version == version) {
        stick.names = names;
    }
    return names;
}

void noteKeptAnalysisPlaylist(const std::string &stickId, const fs::path &root, const std::string &playlist,
                  std::vector<std::string> names)
{
    NameCache &cache = nameCache();
    std::lock_guard lock(cache.mutex);
    StickNames &stick = cache.byStick[stickId];
    if (rootKey(stick.root) != rootKey(root)) {
        stick = StickNames{root, nullptr, stick.version + 1, {}};
    }
    stick.playlists[playlist] = std::move(names);
}

std::shared_ptr<const KeptAnalysisNames> keptAnalysisNamesOf(const std::string &stickId, const fs::path &stickRoot)
{
    NameCache &cache = nameCache();
    const std::string id = stickId.empty() ? "root:" + rootKey(stickRoot) : stickId;
    {
        std::lock_guard lock(cache.mutex);
        if (const auto found = cache.byStick.find(id);
            found != cache.byStick.end() && found->second.names && rootKey(found->second.root) == rootKey(stickRoot)) {
            return found->second.names;
        }
    }
    return relistKeptAnalysisNames(id, stickRoot);
}

void noteKeptAnalysisFilesChanged(const fs::path &stickRoot, const std::set<std::string> &files)
{
    const fs::path rekordboxDir = infrastructure::paths::stickKeptRekordboxAnalysisDir(stickRoot);
    const fs::path engineDir = infrastructure::paths::stickKeptEngineAnalysisDir(stickRoot);
    NameCache &cache = nameCache();
    std::lock_guard lock(cache.mutex);
    for (auto &[id, stick] : cache.byStick) {
        if (rootKey(stick.root) != rootKey(stickRoot)) {
            continue;
        }
        ++stick.version;
        if (!stick.names) {
            continue;
        }
        auto names = std::make_shared<KeptAnalysisNames>(*stick.names);
        for (const std::string &file : files) {
            const fs::path path = pathFromUtf8(file);
            const std::string dir = rootKey(path.parent_path());
            std::set<std::string> *into = dir == rootKey(rekordboxDir) ? &names->rekordbox
                                          : dir == rootKey(engineDir)  ? &names->engine
                                                                       : nullptr;
            if (into == nullptr) {
                continue;
            }
            std::error_code ec;
            const std::string name = pathToUtf8(path.filename());
            if (fs::exists(fs::symlink_status(path, ec))) {
                into->insert(name);
            } else {
                into->erase(name);
            }
        }
        stick.names = std::move(names);
    }
}

void forgetKeptAnalysisNames(const std::string &stickId)
{
    NameCache &cache = nameCache();
    std::lock_guard lock(cache.mutex);
    cache.byStick.erase(stickId);
}

void forgetKeptAnalysisNames()
{
    NameCache &cache = nameCache();
    std::lock_guard lock(cache.mutex);
    cache.byStick.clear();
}

int keptCountFromCache(const std::string &stickId, const std::string &playlist)
{
    NameCache &cache = nameCache();
    std::lock_guard lock(cache.mutex);
    const auto stick = cache.byStick.find(stickId);
    if (stick == cache.byStick.end() || !stick->second.names) {
        return -1;
    }
    // Nothing of rekordbox's kept: no playlist has an original.
    if (stick->second.names->rekordbox.empty()) {
        return 0;
    }
    const auto listed = stick->second.playlists.find(playlist);
    if (listed == stick->second.playlists.end()) {
        return -1;
    }
    int count = 0;
    for (const std::string &name : listed->second) {
        count += stick->second.names->rekordbox.count(name) > 0 ? 1 : 0;
    }
    return count;
}

}  // namespace seabass::gui
