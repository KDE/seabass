// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <iostream>
#include <djinterop/djinterop.hpp>

int main(int argc, char **argv)
{
    auto db = djinterop::engine::load_database(argv[1]);
    for (auto &tr : db.tracks()) {
        try {
            auto title = tr.title();
            if (!title || title->find(argv[2]) == std::string::npos) {
                continue;
            }
            std::cout << "id=" << tr.id() << " title=" << *title << " filename=" << tr.filename() << "\n";
            auto hotCues = tr.hot_cues();
            int count = 0;
            for (size_t i = 0; i < hotCues.size(); ++i) {
                if (hotCues[i]) {
                    count++;
                    std::cout << "  slot " << i << ": \"" << hotCues[i]->label << "\" @ " << hotCues[i]->sample_offset
                              << "\n";
                }
            }
            std::cout << "  total hot cues: " << count << "\n";
        } catch (const std::exception &e) {
            std::cout << "  (error reading track id=" << tr.id() << ": " << e.what() << ")\n";
        }
    }
    return 0;
}
