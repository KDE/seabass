// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/stick_backup/stat_diff.hpp"

using namespace seabass::infrastructure::stick_backup;

namespace
{

TreeEntry file(const std::string &path, std::uint64_t size, std::int64_t mtime)
{
    return {path, false, size, mtime};
}

TreeEntry dir(const std::string &path, std::int64_t mtime)
{
    return {path, true, 0, mtime};
}

ManifestRow row(const std::string &path, std::uint64_t size, std::int64_t mtime, bool directory = false)
{
    ManifestRow r;
    r.kind = directory ? ManifestRow::Kind::Directory : ManifestRow::Kind::File;
    r.path = path;
    r.size = size;
    r.mtimeUnix = mtime;
    return r;
}

std::vector<std::string> paths(const std::vector<const TreeEntry *> &entries)
{
    std::vector<std::string> out;
    for (const TreeEntry *e : entries) {
        out.push_back(e->relativePath);
    }
    return out;
}

}  // namespace

int main()
{
    // ---- No previous backup: everything is added ----
    {
        TreeWalk walk;
        walk.entries = {dir("d", 1), file("d/a", 10, 100), file("b", 20, 200)};
        DiffResult diff = diffTreeAgainstManifest(walk, nullptr);
        assert(diff.added.size() == 3 && diff.unchanged.empty() && diff.changed.empty() && diff.removed.empty());
        assert(diff.bytesToRead == 30);
        std::cout << "case 1 (no previous manifest: all added) OK\n";
    }

    // ---- The classification matrix ----
    {
        BackupManifest previous;
        previous.rows = {
            row("same", 10, 1000),
            row("within-window", 10, 1000),
            row("size-moved", 10, 1000),
            row("mtime-moved", 10, 1000),
            row("gone", 10, 1000),
            row("was-dir", 0, 1000, true),
            row("stays-dir", 0, 1000, true),
        };
        TreeWalk walk;
        walk.entries = {
            file("same", 10, 1000),
            file("within-window", 10, 1002),  // FAT's 2 s resolution
            file("size-moved", 11, 1000),
            file("mtime-moved", 10, 1010),
            file("brand-new", 5, 1),
            file("was-dir", 3, 1000),         // kind changed: added + removed
            dir("stays-dir", 9999),           // directory mtime is ignored
        };
        DiffResult diff = diffTreeAgainstManifest(walk, &previous);
        assert((paths(diff.unchanged) == std::vector<std::string>{"same", "within-window", "stays-dir"}));
        assert((paths(diff.changed) == std::vector<std::string>{"size-moved", "mtime-moved"}));
        assert((paths(diff.added) == std::vector<std::string>{"brand-new", "was-dir"}));
        assert((diff.removed == std::vector<std::string>{"gone", "was-dir"}));
        assert(diff.bytesToRead == 11 + 10 + 5 + 3);
        assert(diff.uniformShiftSeconds == 0);
        std::cout << "case 2 (unchanged / 2 s window / size / mtime / added / removed / kind change / dir mtime ignored) OK\n";
    }

    // ---- Uniform timezone shift: the clock moved, not the files ----
    {
        BackupManifest previous;
        TreeWalk walk;
        for (int i = 0; i < 20; ++i) {
            std::string p = "t/" + std::to_string(i);
            previous.rows.push_back(row(p, 100, 10'000 + i * 10));
            walk.entries.push_back(file(p, 100, 10'000 + i * 10 + 3600 + (i % 2)));  // +1 h, jittered by FAT rounding
        }
        previous.rows.push_back(row("really-changed", 100, 500));
        walk.entries.push_back(file("really-changed", 100, 500 + 7200));  // a different delta: not part of the shift
        previous.rows.push_back(row("grew", 100, 600));
        walk.entries.push_back(file("grew", 101, 600 + 3600));  // size moved too: changed regardless
        DiffResult diff = diffTreeAgainstManifest(walk, &previous);
        assert(diff.uniformShiftSeconds == 3600);
        assert(diff.uniformlyShiftedFiles == 20);
        assert(diff.unchanged.size() == 20);
        assert((paths(diff.changed) == std::vector<std::string>{"grew", "really-changed"})
               || (paths(diff.changed) == std::vector<std::string>{"really-changed", "grew"}));
        assert(diff.bytesToRead == 201);
        std::cout << "case 3 (uniform +1 h shift on 20 files treated as unchanged; outliers still changed) OK\n";
    }

    // ---- Not a shift: too few files, random deltas, or a non-zone delta ----
    {
        {
            BackupManifest previous;
            TreeWalk walk;
            for (int i = 0; i < 5; ++i) {  // below the detection minimum
                std::string p = "f/" + std::to_string(i);
                previous.rows.push_back(row(p, 100, 1000));
                walk.entries.push_back(file(p, 100, 4600));
            }
            DiffResult diff = diffTreeAgainstManifest(walk, &previous);
            assert(diff.uniformShiftSeconds == 0 && diff.changed.size() == 5);
        }
        {
            BackupManifest previous;
            TreeWalk walk;
            for (int i = 0; i < 20; ++i) {
                std::string p = "r/" + std::to_string(i);
                previous.rows.push_back(row(p, 100, 1000));
                walk.entries.push_back(file(p, 100, 1000 + 100 * (i + 1)));  // all different
            }
            DiffResult diff = diffTreeAgainstManifest(walk, &previous);
            assert(diff.uniformShiftSeconds == 0 && diff.changed.size() == 20);
        }
        {
            BackupManifest previous;
            TreeWalk walk;
            for (int i = 0; i < 20; ++i) {
                std::string p = "q/" + std::to_string(i);
                previous.rows.push_back(row(p, 100, 1000));
                walk.entries.push_back(file(p, 100, 1000 + 1000));  // uniform, but no timezone is 1000 s off
            }
            DiffResult diff = diffTreeAgainstManifest(walk, &previous);
            assert(diff.uniformShiftSeconds == 0 && diff.changed.size() == 20);
        }
        std::cout << "case 4 (too few / random / non-zone deltas are real changes) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
