// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "playlist_diff_controller.hpp"

#include <QClipboard>
#include <QGuiApplication>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include "application/path_key.hpp"
#include "domain/track_matching.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/stick_path.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"

namespace seabass::gui
{

namespace
{

// Lines of identical rows kept visible on each side of a fold, and the
// fewest lines a fold must hide to be worth a row of its own.
constexpr int kFoldContext = 2;
constexpr int kFoldMinimum = 3;

// Up to this many table cells (entries of A times entries of B, about
// 2000 x 2000) the diff runs on the GUI thread: tens of milliseconds.
// Beyond, a worker.
constexpr double kSynchronousDiffCells = 4.0e6;

QString kindName(domain::DiffEntryKind kind)
{
    switch (kind) {
    case domain::DiffEntryKind::None:
        return QString();
    case domain::DiffEntryKind::Same:
        return QStringLiteral("same");
    case domain::DiffEntryKind::Only:
        return QStringLiteral("only");
    case domain::DiffEntryKind::Moved:
        return QStringLiteral("moved");
    }
    return QString();
}

QString lineOf(const domain::Track &track)
{
    const QString title = QString::fromStdString(track.title);
    const QString artist = QString::fromStdString(track.artist);
    return artist.isEmpty() ? title : artist + QStringLiteral(" - ") + title;
}

}  // namespace

PlaylistDiffRowModel::PlaylistDiffRowModel(QObject *parent) : QAbstractListModel(parent) {}

int PlaylistDiffRowModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant PlaylistDiffRowModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const Row &row = m_rows[static_cast<std::size_t>(index.row())];
    switch (role) {
    case KindRole:
        return row.kind;
    case LeftKindRole:
        return row.leftKind;
    case LeftPositionRole:
        return row.leftPosition;
    case LeftTitleRole:
        return row.leftTitle;
    case LeftArtistRole:
        return row.leftArtist;
    case LeftBpmRole:
        return row.leftBpm;
    case LeftKeyRole:
        return row.leftKey;
    case LeftDurationSecondsRole:
        return row.leftDurationSeconds;
    case LeftPartnerPositionRole:
        return row.leftPartnerPosition;
    case RightKindRole:
        return row.rightKind;
    case RightPositionRole:
        return row.rightPosition;
    case RightTitleRole:
        return row.rightTitle;
    case RightArtistRole:
        return row.rightArtist;
    case RightBpmRole:
        return row.rightBpm;
    case RightKeyRole:
        return row.rightKey;
    case RightDurationSecondsRole:
        return row.rightDurationSeconds;
    case RightPartnerPositionRole:
        return row.rightPartnerPosition;
    case PartnerRowRole:
        return row.partnerRow;
    case FoldCountRole:
        return row.foldCount;
    }
    return {};
}

QHash<int, QByteArray> PlaylistDiffRowModel::roleNames() const
{
    return {
        {KindRole, "kind"},
        {LeftKindRole, "leftKind"},
        {LeftPositionRole, "leftPosition"},
        {LeftTitleRole, "leftTitle"},
        {LeftArtistRole, "leftArtist"},
        {LeftBpmRole, "leftBpm"},
        {LeftKeyRole, "leftKey"},
        {LeftDurationSecondsRole, "leftDurationSeconds"},
        {LeftPartnerPositionRole, "leftPartnerPosition"},
        {RightKindRole, "rightKind"},
        {RightPositionRole, "rightPosition"},
        {RightTitleRole, "rightTitle"},
        {RightArtistRole, "rightArtist"},
        {RightBpmRole, "rightBpm"},
        {RightKeyRole, "rightKey"},
        {RightDurationSecondsRole, "rightDurationSeconds"},
        {RightPartnerPositionRole, "rightPartnerPosition"},
        {PartnerRowRole, "partnerRow"},
        {FoldCountRole, "foldCount"},
    };
}

void PlaylistDiffRowModel::setRows(std::vector<Row> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

PlaylistDiffController::PlaylistDiffController(QObject *parent) : QObject(parent) {}

namespace
{

// The order the catalogs are read, matched and offered in.
const std::array<const char *, 3> kFormats = {"rekordbox", "onelibrary", "engine"};

bool isFormat(const QString &format)
{
    return std::any_of(kFormats.begin(), kFormats.end(), [&](const char *f) { return format == QLatin1String(f); });
}

// A union-find over every track of every catalog, by (catalog, row).
struct Identities
{
    std::vector<std::size_t> parent;
    std::size_t find(std::size_t x)
    {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    // The smaller index wins, so the identity is the first catalog's row
    // in kFormats order, whatever order the pairs arrive in.
    void join(std::size_t x, std::size_t y)
    {
        x = find(x);
        y = find(y);
        if (x != y) {
            parent[std::max(x, y)] = std::min(x, y);
        }
    }
};

}  // namespace

QString PlaylistDiffController::catalogLabel(const QString &format)
{
    if (format == QLatin1String("rekordbox")) {
        return QStringLiteral("DeviceLibrary");
    }
    if (format == QLatin1String("onelibrary")) {
        return QStringLiteral("OneLibrary");
    }
    if (format == QLatin1String("engine")) {
        return QStringLiteral("Engine");
    }
    return QString();
}

QStringList PlaylistDiffController::formats() const
{
    QStringList out;
    for (const char *f : kFormats) {
        if (m_catalogs.count(QLatin1String(f))) {
            out << QLatin1String(f);
        }
    }
    return out;
}

const PlaylistDiffController::Catalog *PlaylistDiffController::catalogOf(const QString &format) const
{
    auto it = m_catalogs.find(format);
    return it == m_catalogs.end() ? nullptr : &it->second;
}

QStringList PlaylistDiffController::namesOf(const QString &format) const
{
    const Catalog *catalog = catalogOf(format);
    return catalog ? catalog->names : QStringList();
}

QVariantMap PlaylistDiffController::countsOf(const QString &format) const
{
    QVariantMap counts;
    if (const Catalog *catalog = catalogOf(format)) {
        for (const auto &[name, entries] : catalog->playlists) {
            counts[name] = static_cast<int>(entries.size());
        }
    }
    return counts;
}

bool PlaylistDiffController::hasOneLibrary(const QString &pioneerRoot) const
{
    return !pioneerRoot.isEmpty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneerRoot.toStdString());
}

void PlaylistDiffController::scan(const QString &rekordboxPath, const QString &enginePath)
{
    setErrorMessage({});
    // The catalogs shown so far are another stick's, or this one's as it
    // was: gone now, not left on the page while this scan runs, nor for
    // good if it fails or is cancelled.
    clearCatalogs();
    std::vector<std::pair<std::string, std::string>> reads;  // (format, path)
    if (!rekordboxPath.isEmpty()) {
        const std::string pioneer = rekordboxPath.toStdString();
        reads.emplace_back("rekordbox", pioneer);
        if (hasOneLibrary(rekordboxPath)) {
            reads.emplace_back("onelibrary", pioneer);
        }
    }
    if (!enginePath.isEmpty()) {
        reads.emplace_back("engine", enginePath.toStdString());
    }
    if (reads.empty()) {
        setErrorMessage(QStringLiteral("This stick has no library to compare."));
        return;
    }
    m_scan.start(
        rekordboxPath + QLatin1Char('\n') + enginePath, stickRootOf(rekordboxPath.isEmpty() ? enginePath : rekordboxPath),
        [reads](application::CancellationToken cancel) {
            ScanResult result;
            for (const auto &[format, path] : reads) {
                try {
                    // The catalog alone: titles, artists, file paths and
                    // memberships are all a diff needs, and this stage is
                    // the quick one.
                    Catalog catalog;
                    catalog.tracks = LibraryCatalogCache::instance().tracksFor(
                        format, path, LibraryCatalogCache::Detail::Tracks,
                        application::NullProgressReporter::instance(), cancel);
                    result.catalogs.emplace(QString::fromStdString(format), std::move(catalog));
                } catch (const application::OperationCancelled &) {
                    result.cancelled = true;
                    return result;
                } catch (const std::exception &e) {
                    result.failures << QStringLiteral("%1: %2").arg(
                        catalogLabel(QString::fromStdString(format)), QString::fromStdString(e.what()));
                }
            }
            for (auto &[format, catalog] : result.catalogs) {
                indexPlaylists(catalog);
            }
            resolveIdentities(result.catalogs);
            return result;
        },
        {
            [this](ScanResult &&result) { onScanFinished(std::move(result)); },
            [this](const QString &message) { setErrorMessage(message); },
            [this]() { emit scanCancelled(); },
        });
}

void PlaylistDiffController::clearCatalogs()
{
    m_diffJob.cancel();
    m_catalogs.clear();
    m_diff = {};
    m_openFolds.clear();
    m_relatives.clear();
    m_verdict.clear();
    rebuildRows();
    emit playlistsChanged();
    emit diffChanged();
}

void PlaylistDiffController::cancelScan()
{
    m_scan.cancel();
}

void PlaylistDiffController::onScanFinished(ScanResult &&result)
{
    if (result.cancelled) {
        emit scanCancelled();
        return;
    }
    // A catalog that could not be read is said so, and the others are
    // still there to compare.
    setErrorMessage(result.failures.join(QLatin1Char('\n')));
    if (!result.catalogs.empty()) {
        setCatalogs(std::move(result.catalogs));
    }
}

void PlaylistDiffController::setTracksForTesting(std::vector<domain::Track> tracks, const QString &format)
{
    std::map<QString, Catalog> catalogs;
    Catalog catalog;
    catalog.tracks = std::move(tracks);
    indexPlaylists(catalog);
    catalogs.emplace(format, std::move(catalog));
    resolveIdentities(catalogs);
    setCatalogs(std::move(catalogs));
}

void PlaylistDiffController::setCatalogs(std::map<QString, Catalog> catalogs)
{
    m_catalogs = std::move(catalogs);
    chooseDefaults(true);
    emit playlistsChanged();
    emit selectionChanged();
    recompute();
}

void PlaylistDiffController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

// One identity per track, shared by the rows of the other catalogs
// matchTracks pairs it with: the pairing Sync and every other page
// across two catalogs use, within one stick. Paths are compared as
// normalizedPathKey has them, so a padded DeviceLibrary path and Engine's
// spelling of the same file meet. A track no other catalog has a row for
// keeps an identity of its own.
void PlaylistDiffController::resolveIdentities(std::map<QString, Catalog> &catalogs)
{
    std::vector<Catalog *> ordered;
    std::vector<std::size_t> offsets;
    std::size_t total = 0;
    for (const char *f : kFormats) {
        auto it = catalogs.find(QLatin1String(f));
        if (it != catalogs.end()) {
            ordered.push_back(&it->second);
            offsets.push_back(total);
            total += it->second.tracks.size();
        }
    }
    Identities identities;
    identities.parent.resize(total);
    for (std::size_t i = 0; i < total; ++i) {
        identities.parent[i] = i;
    }
    // Copies with their paths as keys: matchTracks compares paths as
    // strings.
    std::vector<std::vector<domain::Track>> keyed;
    keyed.reserve(ordered.size());
    for (const Catalog *catalog : ordered) {
        std::vector<domain::Track> copy = catalog->tracks;
        for (domain::Track &track : copy) {
            if (!track.filePath.empty()) {
                track.filePath = application::normalizedPathKey(track.filePath);
            }
        }
        keyed.push_back(std::move(copy));
    }
    for (std::size_t x = 0; x < keyed.size(); ++x) {
        for (std::size_t y = x + 1; y < keyed.size(); ++y) {
            const auto &a = keyed[x];
            const auto &b = keyed[y];
            for (const auto &[trackA, trackB] : domain::matchTracks(a, b, domain::MatchScope::OneStick)) {
                identities.join(offsets[x] + static_cast<std::size_t>(trackA - a.data()),
                                offsets[y] + static_cast<std::size_t>(trackB - b.data()));
            }
        }
    }
    // The name of a root: its catalog and row, unique across catalogs.
    std::vector<std::string> rootNames(total);
    for (std::size_t c = 0; c < ordered.size(); ++c) {
        for (std::size_t i = 0; i < ordered[c]->tracks.size(); ++i) {
            rootNames[offsets[c] + i] = std::to_string(c) + ":" + ordered[c]->tracks[i].sourceId;
        }
    }
    for (std::size_t c = 0; c < ordered.size(); ++c) {
        Catalog &catalog = *ordered[c];
        catalog.identities.resize(catalog.tracks.size());
        for (std::size_t i = 0; i < catalog.tracks.size(); ++i) {
            catalog.identities[i] = rootNames[identities.find(offsets[c] + i)];
        }
    }
}

// A playlist is the tracks that name it, in membership order. The same
// rule Browse Library's list uses: an empty playlist has no track to
// name it, so it is not here.
void PlaylistDiffController::indexPlaylists(Catalog &catalog)
{
    std::map<QString, std::vector<std::pair<int, int>>> positioned;  // name -> (position, track index)
    for (int i = 0; i < static_cast<int>(catalog.tracks.size()); ++i) {
        for (const auto &membership : catalog.tracks[static_cast<std::size_t>(i)].playlists) {
            positioned[QString::fromStdString(membership.name)].emplace_back(membership.position, i);
        }
    }
    catalog.playlists.clear();
    catalog.names.clear();
    for (auto &[name, list] : positioned) {
        // Stable: entries without a position (-1) keep catalog order.
        std::stable_sort(list.begin(), list.end(),
                         [](const auto &x, const auto &y) { return x.first < y.first; });
        Entries entries;
        entries.reserve(list.size());
        for (const auto &[position, index] : list) {
            entries.push_back(index);
        }
        catalog.playlists.emplace(name, std::move(entries));
        catalog.names.push_back(name);
    }
    std::sort(catalog.names.begin(), catalog.names.end(),
              [](const QString &x, const QString &y) { return x.compare(y, Qt::CaseInsensitive) < 0; });
}

// A side whose catalog was not read falls back to the stick's Engine
// library, else the first one there is; a playlist its catalog does not
// have falls back to the first playlist for A and A's nearest relative
// for B, so the page opens on a diff worth looking at rather than a
// blank one.
void PlaylistDiffController::chooseDefaults(bool avoidSamePair)
{
    if (m_catalogs.empty()) {
        return;
    }
    const QString fallback = m_catalogs.count(QStringLiteral("engine")) ? QStringLiteral("engine") : formats().first();
    if (!m_catalogs.count(m_formatA)) {
        m_formatA = fallback;
    }
    if (!m_catalogs.count(m_formatB)) {
        m_formatB = fallback;
    }
    const Catalog &a = *catalogOf(m_formatA);
    const Catalog &b = *catalogOf(m_formatB);
    if (!a.playlists.count(m_playlistA)) {
        m_playlistA = a.names.isEmpty() ? QString() : a.names.first();
    }
    const bool samePair = m_formatA == m_formatB && m_playlistB == m_playlistA;
    if (!b.playlists.count(m_playlistB) || (avoidSamePair && samePair)) {
        const QString best = nearestRelativeInB();
        m_playlistB = !best.isEmpty() ? best : b.playlists.count(m_playlistA) ? m_playlistA
                                          : b.names.isEmpty()                ? QString()
                                                                             : b.names.first();
    }
}

QString PlaylistDiffController::nearestRelativeInB() const
{
    const Catalog *b = catalogOf(m_formatB);
    if (!b) {
        return {};
    }
    const bool across = m_formatA != m_formatB;
    const auto idsA = idsOf(m_formatA, entriesOf(m_formatA, m_playlistA), across);
    QString best;
    double bestScore = -1.0;
    for (const QString &name : b->names) {
        if (!across && name == m_playlistA) {
            continue;
        }
        const auto overlap = domain::overlapOf(idsA, idsOf(m_formatB, entriesOf(m_formatB, name), across));
        if (overlap.jaccard > bestScore) {
            bestScore = overlap.jaccard;
            best = name;
        }
    }
    return best;
}

const PlaylistDiffController::Entries &PlaylistDiffController::entriesOf(const QString &format,
                                                                          const QString &name) const
{
    static const Entries none;
    const Catalog *catalog = catalogOf(format);
    if (!catalog) {
        return none;
    }
    auto it = catalog->playlists.find(name);
    return it == catalog->playlists.end() ? none : it->second;
}

std::vector<std::string> PlaylistDiffController::idsOf(const QString &format, const Entries &entries,
                                                       bool acrossCatalogs) const
{
    std::vector<std::string> ids;
    const Catalog *catalog = catalogOf(format);
    if (!catalog) {
        return ids;
    }
    ids.reserve(entries.size());
    for (int index : entries) {
        const auto i = static_cast<std::size_t>(index);
        ids.push_back(acrossCatalogs ? catalog->identities[i] : catalog->tracks[i].sourceId);
    }
    return ids;
}

const domain::Track &PlaylistDiffController::trackOf(const QString &format, int index) const
{
    return catalogOf(format)->tracks[static_cast<std::size_t>(index)];
}

QString PlaylistDiffController::sideName(bool sideA) const
{
    const QString &name = sideA ? m_playlistA : m_playlistB;
    if (m_formatA == m_formatB) {
        return name;
    }
    return QStringLiteral("%1 (%2)").arg(name, catalogLabel(sideA ? m_formatA : m_formatB));
}

void PlaylistDiffController::setFormatA(const QString &format)
{
    if (format == m_formatA || !isFormat(format)) {
        return;
    }
    m_formatA = format;
    if (!m_catalogs.empty()) {
        chooseDefaults(false);
    }
    emit playlistsChanged();
    emit selectionChanged();
    recompute();
}

void PlaylistDiffController::setFormatB(const QString &format)
{
    if (format == m_formatB || !isFormat(format)) {
        return;
    }
    m_formatB = format;
    if (!m_catalogs.empty()) {
        chooseDefaults(false);
    }
    emit playlistsChanged();
    emit selectionChanged();
    recompute();
}

void PlaylistDiffController::chooseB(const QString &format, const QString &name)
{
    if (!isFormat(format) || (format == m_formatB && name == m_playlistB)) {
        return;
    }
    const bool formatChanged = format != m_formatB;
    m_formatB = format;
    m_playlistB = name;
    if (formatChanged) {
        emit playlistsChanged();
    }
    emit selectionChanged();
    recompute();
}

void PlaylistDiffController::setPlaylistA(const QString &name)
{
    if (name == m_playlistA) {
        return;
    }
    m_playlistA = name;
    emit selectionChanged();
    recompute();
}

void PlaylistDiffController::setPlaylistB(const QString &name)
{
    if (name == m_playlistB) {
        return;
    }
    m_playlistB = name;
    emit selectionChanged();
    recompute();
}

void PlaylistDiffController::swapPlaylists()
{
    std::swap(m_formatA, m_formatB);
    std::swap(m_playlistA, m_playlistB);
    if (m_formatA != m_formatB) {
        emit playlistsChanged();
    }
    emit selectionChanged();
    recompute();
}

void PlaylistDiffController::setFoldIdentical(bool fold)
{
    if (fold == m_foldIdentical) {
        return;
    }
    m_foldIdentical = fold;
    m_openFolds.clear();
    emit foldIdenticalChanged();
    rebuildRows();
}

void PlaylistDiffController::expandFold(int row)
{
    const auto &rows = m_rows.rows();
    if (row < 0 || row >= static_cast<int>(rows.size()) || rows[static_cast<std::size_t>(row)].foldStart < 0) {
        return;
    }
    m_openFolds.push_back(rows[static_cast<std::size_t>(row)].foldStart);
    rebuildRows();
}

// The relatives now, the diff now when it is small and on a worker when
// it is not: the linear-space diff is O(n*m) time, seconds for two
// reshuffled 20000-entry playlists in a debug build.
void PlaylistDiffController::recompute()
{
    m_openFolds.clear();

    // The relatives of A in every catalog read, most alike first; A's own
    // catalog first among equals.
    m_relatives.clear();
    if (!m_playlistA.isEmpty() && catalogOf(m_formatA)) {
        struct Relative
        {
            QString format;
            QString name;
            int count = 0;
            domain::PlaylistOverlap overlap;
        };
        std::vector<Relative> relatives;
        QStringList order{m_formatA};
        for (const QString &f : formats()) {
            if (f != m_formatA) {
                order << f;
            }
        }
        const Entries &a = entriesOf(m_formatA, m_playlistA);
        const auto idsWithin = idsOf(m_formatA, a, false);
        const auto idsAcross = idsOf(m_formatA, a, true);
        for (const QString &format : order) {
            const bool across = format != m_formatA;
            const Catalog &catalog = *catalogOf(format);
            for (const QString &name : catalog.names) {
                if (!across && name == m_playlistA) {
                    continue;
                }
                const Entries &entries = entriesOf(format, name);
                const auto overlap = domain::overlapOf(across ? idsAcross : idsWithin, idsOf(format, entries, across));
                if (overlap.relation != domain::PlaylistRelation::Disjoint) {
                    relatives.push_back({format, name, static_cast<int>(entries.size()), overlap});
                }
            }
        }
        std::stable_sort(relatives.begin(), relatives.end(),
                         [](const Relative &x, const Relative &y) { return x.overlap.jaccard > y.overlap.jaccard; });
        for (const Relative &r : relatives) {
            QVariantMap m;
            m["name"] = r.name;
            m["format"] = r.format;
            m["catalog"] = catalogLabel(r.format);
            m["label"] = r.format == m_formatA ? r.name : QStringLiteral("%1: %2").arg(catalogLabel(r.format), r.name);
            m["count"] = r.count;
            m["shared"] = r.overlap.shared;
            m["onlyA"] = r.overlap.onlyA;
            m["onlyB"] = r.overlap.onlyB;
            switch (r.overlap.relation) {
            case domain::PlaylistRelation::Identical:
                m["relation"] = QStringLiteral("identical");
                m["glyph"] = QStringLiteral("=");
                m["detail"] = QStringLiteral("the same tracks");
                break;
            case domain::PlaylistRelation::Superset:
                m["relation"] = QStringLiteral("superset");
                m["glyph"] = QStringLiteral("⊃");
                m["detail"] = QStringLiteral("has all of A, plus %1").arg(r.overlap.onlyB);
                break;
            case domain::PlaylistRelation::Subset:
                m["relation"] = QStringLiteral("subset");
                m["glyph"] = QStringLiteral("⊂");
                m["detail"] = QStringLiteral("part of A, %1 fewer").arg(r.overlap.onlyA);
                break;
            default:
                m["relation"] = QStringLiteral("overlap");
                m["glyph"] = QStringLiteral("∩");
                m["detail"] = QStringLiteral("%1 shared, %2 and %3 apart")
                                  .arg(r.overlap.shared)
                                  .arg(r.overlap.onlyA)
                                  .arg(r.overlap.onlyB);
                break;
            }
            m_relatives.push_back(m);
        }
    }

    const bool across = m_formatA != m_formatB;
    std::vector<std::string> idsA = idsOf(m_formatA, entriesOf(m_formatA, m_playlistA), across);
    std::vector<std::string> idsB = idsOf(m_formatB, entriesOf(m_formatB, m_playlistB), across);
    const double cells = static_cast<double>(idsA.size()) * static_cast<double>(idsB.size());
    if (cells <= kSynchronousDiffCells) {
        // A big diff still running is for another pair: its answer must
        // never land on this one.
        m_diffJob.cancel();
        applyDiff(domain::diffPlaylists(idsA, idsB));
        return;
    }
    // Until the worker answers, no rows: never the previous pair's rows
    // under the new names. A newer recompute supersedes this request,
    // and a superseded answer is swallowed (see AsyncRequest, rule 4).
    m_diff = {};
    m_verdict = QStringLiteral("Comparing two long playlists: %1 and %2 entries.")
                    .arg(QString::number(idsA.size()), QString::number(idsB.size()));
    rebuildRows();
    emit diffChanged();
    m_diffJob.restart(
        QString::number(++m_diffGeneration), QString(),
        [idsA = std::move(idsA), idsB = std::move(idsB)](application::CancellationToken cancel) {
            return domain::diffPlaylists(idsA, idsB, [cancel]() { return cancel.cancelled(); });
        },
        {
            [this](domain::PlaylistDiff &&diff) { applyDiff(std::move(diff)); },
            [this](const QString &message) { setErrorMessage(message); },
            {},
        });
}

void PlaylistDiffController::applyDiff(domain::PlaylistDiff diff)
{
    m_diff = std::move(diff);
    m_openFolds.clear();

    // The verdict, in words a reader can act on.
    auto plural = [](int n, const char *word) {
        return QStringLiteral("%1 %2%3").arg(n).arg(QLatin1String(word)).arg(n == 1 ? "" : "s");
    };
    // n tracks only one side has, saying how many of them are extra
    // copies of a track the other side lists fewer times.
    auto tracksOnly = [&plural](int n, int extra) {
        if (extra == 0) {
            return plural(n, "track");
        }
        const QString copies = extra == 1 ? QStringLiteral("1 extra copy") : QStringLiteral("%1 extra copies").arg(extra);
        return extra == n ? copies : QStringLiteral("%1 (%2)").arg(plural(n, "track"), copies);
    };
    const QString nameA = sideName(true);
    const QString nameB = sideName(false);
    const QString reordered = m_diff.moved > 0 ? QStringLiteral(", %1 reordered").arg(m_diff.moved) : QString();
    if (m_playlistA.isEmpty() || m_playlistB.isEmpty()) {
        m_verdict = QStringLiteral("This catalog has no playlists to compare.");
    } else if (m_formatA == m_formatB && m_playlistA == m_playlistB) {
        m_verdict = QStringLiteral("The same playlist on both sides.");
    } else if (m_diff.identical()) {
        m_verdict = QStringLiteral("Identical: the same %1 in the same order.").arg(plural(m_diff.shared, "track"));
    } else if (m_diff.sameTracks()) {
        m_verdict = QStringLiteral("The same %1, %2 of them in a different order.")
                        .arg(plural(m_diff.shared, "track"))
                        .arg(m_diff.moved);
    } else if (m_diff.onlyA == 0) {
        m_verdict = QStringLiteral("%1 is %2 plus %3%4.")
                        .arg(nameB, nameA, tracksOnly(m_diff.onlyB, m_diff.extraB), reordered);
    } else if (m_diff.onlyB == 0) {
        m_verdict = QStringLiteral("%1 is %2 minus %3%4.")
                        .arg(nameB, nameA, tracksOnly(m_diff.onlyA, m_diff.extraA), reordered);
    } else {
        // One pass: a name with "%1" in it is text, not a placeholder.
        m_verdict = QStringLiteral("%1 shared. %2 has %3 the other lacks, %4 has %5%6.")
                        .arg(QString::number(m_diff.shared), nameA, QString::number(m_diff.onlyA), nameB,
                             QString::number(m_diff.onlyB), reordered);
    }

    rebuildRows();
    emit diffChanged();
}

void PlaylistDiffController::rebuildRows()
{
    using Row = PlaylistDiffRowModel::Row;
    const Entries &a = entriesOf(m_formatA, m_playlistA);
    const Entries &b = entriesOf(m_formatB, m_playlistB);
    const auto &unfolded = m_diff.rows;

    auto fillLeft = [&](Row &row, const domain::DiffEntry &entry) {
        row.leftKind = kindName(entry.kind);
        if (entry.kind == domain::DiffEntryKind::None) {
            return;
        }
        const domain::Track &t = trackOf(m_formatA, a[static_cast<std::size_t>(entry.index)]);
        row.leftPosition = entry.index + 1;
        row.leftTitle = QString::fromStdString(t.title);
        row.leftArtist = QString::fromStdString(t.artist);
        row.leftBpm = t.bpm;
        row.leftKey = QString::fromStdString(t.key);
        row.leftDurationSeconds = t.durationSeconds;
        row.leftPartnerPosition = entry.partner >= 0 ? entry.partner + 1 : 0;
    };
    auto fillRight = [&](Row &row, const domain::DiffEntry &entry) {
        row.rightKind = kindName(entry.kind);
        if (entry.kind == domain::DiffEntryKind::None) {
            return;
        }
        const domain::Track &t = trackOf(m_formatB, b[static_cast<std::size_t>(entry.index)]);
        row.rightPosition = entry.index + 1;
        row.rightTitle = QString::fromStdString(t.title);
        row.rightArtist = QString::fromStdString(t.artist);
        row.rightBpm = t.bpm;
        row.rightKey = QString::fromStdString(t.key);
        row.rightDurationSeconds = t.durationSeconds;
        row.rightPartnerPosition = entry.partner >= 0 ? entry.partner + 1 : 0;
    };
    auto kindOf = [](const domain::DiffRow &r) {
        using K = domain::DiffEntryKind;
        if (r.a.kind == K::Same) {
            return QStringLiteral("same");
        }
        if (r.a.kind != K::None && r.b.kind != K::None) {
            return QStringLiteral("mixed");
        }
        if (r.a.kind == K::Only) {
            return QStringLiteral("onlyA");
        }
        if (r.a.kind == K::Moved) {
            return QStringLiteral("movedA");
        }
        return r.b.kind == K::Only ? QStringLiteral("onlyB") : QStringLiteral("movedB");
    };

    std::vector<Row> rows;
    const int n = static_cast<int>(unfolded.size());
    int k = 0;
    while (k < n) {
        if (m_foldIdentical && unfolded[static_cast<std::size_t>(k)].same()) {
            int e = k;
            while (e < n && unfolded[static_cast<std::size_t>(e)].same()) {
                ++e;
            }
            const int head = k == 0 ? 0 : kFoldContext;
            const int tail = e == n ? 0 : kFoldContext;
            const int hidden = e - k - head - tail;
            const bool open = std::find(m_openFolds.begin(), m_openFolds.end(), k) != m_openFolds.end();
            if (hidden >= kFoldMinimum && !open) {
                for (int x = k; x < k + head; ++x) {
                    Row row;
                    row.kind = QStringLiteral("same");
                    fillLeft(row, unfolded[static_cast<std::size_t>(x)].a);
                    fillRight(row, unfolded[static_cast<std::size_t>(x)].b);
                    rows.push_back(row);
                }
                Row fold;
                fold.kind = QStringLiteral("fold");
                fold.foldCount = hidden;
                fold.foldStart = k;
                rows.push_back(fold);
                for (int x = e - tail; x < e; ++x) {
                    Row row;
                    row.kind = QStringLiteral("same");
                    fillLeft(row, unfolded[static_cast<std::size_t>(x)].a);
                    fillRight(row, unfolded[static_cast<std::size_t>(x)].b);
                    rows.push_back(row);
                }
                k = e;
                continue;
            }
            // Open, or too short to fold: the whole run, row by row.
            // Leaving it to the loop below would fold again from the
            // run's second row.
            for (int x = k; x < e; ++x) {
                Row row;
                row.kind = QStringLiteral("same");
                fillLeft(row, unfolded[static_cast<std::size_t>(x)].a);
                fillRight(row, unfolded[static_cast<std::size_t>(x)].b);
                rows.push_back(row);
            }
            k = e;
            continue;
        }
        Row row;
        const domain::DiffRow &r = unfolded[static_cast<std::size_t>(k)];
        row.kind = kindOf(r);
        fillLeft(row, r.a);
        fillRight(row, r.b);
        rows.push_back(row);
        ++k;
    }

    // Partner rows: a moved track's other half, by the position it shows.
    std::map<int, int> rowOfLeftPosition;
    std::map<int, int> rowOfRightPosition;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const Row &row = rows[static_cast<std::size_t>(i)];
        if (row.leftKind == QLatin1String("moved")) {
            rowOfLeftPosition[row.leftPosition] = i;
        }
        if (row.rightKind == QLatin1String("moved")) {
            rowOfRightPosition[row.rightPosition] = i;
        }
    }
    for (Row &row : rows) {
        if (row.leftKind == QLatin1String("moved")) {
            auto it = rowOfRightPosition.find(row.leftPartnerPosition);
            row.partnerRow = it == rowOfRightPosition.end() ? -1 : it->second;
        } else if (row.rightKind == QLatin1String("moved")) {
            auto it = rowOfLeftPosition.find(row.rightPartnerPosition);
            row.partnerRow = it == rowOfLeftPosition.end() ? -1 : it->second;
        }
    }
    m_rows.setRows(std::move(rows));
}

// The entries the diff calls only this side's, in list order: a track
// the other list lacks, and an extra copy of one it lists fewer times.
QString PlaylistDiffController::onlyText(bool sideA) const
{
    const QString &format = sideA ? m_formatA : m_formatB;
    const Entries &entries = entriesOf(format, sideA ? m_playlistA : m_playlistB);
    QStringList lines;
    for (const domain::DiffRow &row : m_diff.rows) {
        const domain::DiffEntry &entry = sideA ? row.a : row.b;
        if (entry.kind == domain::DiffEntryKind::Only) {
            lines << lineOf(trackOf(format, entries[static_cast<std::size_t>(entry.index)]));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QString PlaylistDiffController::onlyInAText() const
{
    return onlyText(true);
}

QString PlaylistDiffController::onlyInBText() const
{
    return onlyText(false);
}

void PlaylistDiffController::copyToClipboard(const QString &text) const
{
    if (auto *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text);
    }
}

}  // namespace seabass::gui
