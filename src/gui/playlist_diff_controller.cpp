// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "playlist_diff_controller.hpp"

#include <QClipboard>
#include <QGuiApplication>

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "gui/library_catalog_cache.hpp"
#include "gui/stick_path.hpp"

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

QVariantMap PlaylistDiffController::playlistTrackCounts() const
{
    QVariantMap counts;
    for (const auto &[name, entries] : m_playlists) {
        counts[name] = static_cast<int>(entries.size());
    }
    return counts;
}

void PlaylistDiffController::scan(const QString &format, const QString &path)
{
    setErrorMessage({});
    const std::string fmt = format.toStdString();
    const std::string catalogPath = path.toStdString();
    m_scan.start(
        format + QLatin1Char('\n') + path, stickRootOf(path),
        [fmt, catalogPath](application::CancellationToken cancel) {
            ScanResult result;
            try {
                // The catalog alone: titles, artists and memberships are
                // all a diff needs, and this stage is the quick one.
                result.tracks = LibraryCatalogCache::instance().tracksFor(
                    fmt, catalogPath, LibraryCatalogCache::Detail::Tracks,
                    application::NullProgressReporter::instance(), cancel);
            } catch (const application::OperationCancelled &) {
                result.cancelled = true;
            } catch (const std::exception &e) {
                result.errorMessage = QString::fromStdString(e.what());
            }
            return result;
        },
        {
            [this](ScanResult &&result) { onScanFinished(std::move(result)); },
            [this](const QString &message) { setErrorMessage(message); },
            [this]() { emit scanCancelled(); },
        });
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
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        return;
    }
    setTracks(std::move(result.tracks));
}

void PlaylistDiffController::setTracksForTesting(std::vector<domain::Track> tracks)
{
    setTracks(std::move(tracks));
}

void PlaylistDiffController::setTracks(std::vector<domain::Track> tracks)
{
    m_tracks = std::move(tracks);
    indexPlaylists();
    chooseDefaults();
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

// A playlist is the tracks that name it, in membership order. The same
// rule Browse Library's list uses: an empty playlist has no track to
// name it, so it is not here.
void PlaylistDiffController::indexPlaylists()
{
    std::map<QString, std::vector<std::pair<int, int>>> positioned;  // name -> (position, track index)
    for (int i = 0; i < static_cast<int>(m_tracks.size()); ++i) {
        for (const auto &membership : m_tracks[static_cast<std::size_t>(i)].playlists) {
            positioned[QString::fromStdString(membership.name)].emplace_back(membership.position, i);
        }
    }
    m_playlists.clear();
    m_names.clear();
    for (auto &[name, list] : positioned) {
        // Stable: entries without a position (-1) keep catalog order.
        std::stable_sort(list.begin(), list.end(),
                         [](const auto &x, const auto &y) { return x.first < y.first; });
        Entries entries;
        entries.reserve(list.size());
        for (const auto &[position, index] : list) {
            entries.push_back(index);
        }
        m_playlists.emplace(name, std::move(entries));
        m_names.push_back(name);
    }
    std::sort(m_names.begin(), m_names.end(),
              [](const QString &x, const QString &y) { return x.compare(y, Qt::CaseInsensitive) < 0; });
}

// A and B that no longer exist (or were never set) fall back to the first
// playlist and its nearest relative, so the page opens on a diff worth
// looking at rather than a blank one.
void PlaylistDiffController::chooseDefaults()
{
    if (m_names.isEmpty()) {
        m_playlistA.clear();
        m_playlistB.clear();
        return;
    }
    if (!m_playlists.count(m_playlistA)) {
        m_playlistA = m_names.first();
    }
    if (!m_playlists.count(m_playlistB) || m_playlistB == m_playlistA) {
        QString best;
        double bestScore = -1.0;
        const auto idsA = idsOf(entriesOf(m_playlistA));
        for (const QString &name : m_names) {
            if (name == m_playlistA) {
                continue;
            }
            const auto overlap = domain::overlapOf(idsA, idsOf(entriesOf(name)));
            if (overlap.jaccard > bestScore) {
                bestScore = overlap.jaccard;
                best = name;
            }
        }
        m_playlistB = best.isEmpty() ? m_playlistA : best;
    }
}

const PlaylistDiffController::Entries &PlaylistDiffController::entriesOf(const QString &name) const
{
    static const Entries none;
    auto it = m_playlists.find(name);
    return it == m_playlists.end() ? none : it->second;
}

std::vector<std::string> PlaylistDiffController::idsOf(const Entries &entries) const
{
    std::vector<std::string> ids;
    ids.reserve(entries.size());
    for (int index : entries) {
        ids.push_back(m_tracks[static_cast<std::size_t>(index)].sourceId);
    }
    return ids;
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
    std::swap(m_playlistA, m_playlistB);
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
    const Entries &a = entriesOf(m_playlistA);
    const Entries &b = entriesOf(m_playlistB);
    m_openFolds.clear();

    // The relatives of A, most alike first.
    m_relatives.clear();
    if (!m_playlistA.isEmpty()) {
        const auto idsA = idsOf(a);
        struct Relative
        {
            QString name;
            domain::PlaylistOverlap overlap;
        };
        std::vector<Relative> relatives;
        for (const QString &name : m_names) {
            if (name == m_playlistA) {
                continue;
            }
            const auto overlap = domain::overlapOf(idsA, idsOf(entriesOf(name)));
            if (overlap.relation != domain::PlaylistRelation::Disjoint) {
                relatives.push_back({name, overlap});
            }
        }
        std::stable_sort(relatives.begin(), relatives.end(),
                         [](const Relative &x, const Relative &y) { return x.overlap.jaccard > y.overlap.jaccard; });
        for (const Relative &r : relatives) {
            QVariantMap m;
            m["name"] = r.name;
            m["count"] = static_cast<int>(entriesOf(r.name).size());
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

    std::vector<std::string> idsA = idsOf(a);
    std::vector<std::string> idsB = idsOf(b);
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
    const QString reordered = m_diff.moved > 0 ? QStringLiteral(", %1 reordered").arg(m_diff.moved) : QString();
    if (m_playlistA.isEmpty() || m_playlistB.isEmpty()) {
        m_verdict = QStringLiteral("This catalog has no playlists to compare.");
    } else if (m_playlistA == m_playlistB) {
        m_verdict = QStringLiteral("The same playlist on both sides.");
    } else if (m_diff.identical()) {
        m_verdict = QStringLiteral("Identical: the same %1 in the same order.").arg(plural(m_diff.shared, "track"));
    } else if (m_diff.sameTracks()) {
        m_verdict = QStringLiteral("The same %1, %2 of them in a different order.")
                        .arg(plural(m_diff.shared, "track"))
                        .arg(m_diff.moved);
    } else if (m_diff.onlyA == 0) {
        m_verdict = QStringLiteral("%1 is %2 plus %3%4.")
                        .arg(m_playlistB, m_playlistA, tracksOnly(m_diff.onlyB, m_diff.extraB), reordered);
    } else if (m_diff.onlyB == 0) {
        m_verdict = QStringLiteral("%1 is %2 minus %3%4.")
                        .arg(m_playlistB, m_playlistA, tracksOnly(m_diff.onlyA, m_diff.extraA), reordered);
    } else {
        m_verdict = QStringLiteral("%1 shared. %2 has %3 the other lacks, %4 has %5%6.")
                        .arg(m_diff.shared)
                        .arg(m_playlistA)
                        .arg(m_diff.onlyA)
                        .arg(m_playlistB)
                        .arg(m_diff.onlyB)
                        .arg(reordered);
    }

    rebuildRows();
    emit diffChanged();
}

void PlaylistDiffController::rebuildRows()
{
    using Row = PlaylistDiffRowModel::Row;
    const Entries &a = entriesOf(m_playlistA);
    const Entries &b = entriesOf(m_playlistB);
    const auto &unfolded = m_diff.rows;

    auto fillLeft = [&](Row &row, const domain::DiffEntry &entry) {
        row.leftKind = kindName(entry.kind);
        if (entry.kind == domain::DiffEntryKind::None) {
            return;
        }
        const domain::Track &t = m_tracks[static_cast<std::size_t>(a[static_cast<std::size_t>(entry.index)])];
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
        const domain::Track &t = m_tracks[static_cast<std::size_t>(b[static_cast<std::size_t>(entry.index)])];
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
    const Entries &entries = entriesOf(sideA ? m_playlistA : m_playlistB);
    QStringList lines;
    for (const domain::DiffRow &row : m_diff.rows) {
        const domain::DiffEntry &entry = sideA ? row.a : row.b;
        if (entry.kind == domain::DiffEntryKind::Only) {
            lines << lineOf(m_tracks[static_cast<std::size_t>(entries[static_cast<std::size_t>(entry.index)])]);
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
