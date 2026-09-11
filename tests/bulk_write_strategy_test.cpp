// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "infrastructure/bulk_write_strategy.hpp"

using namespace seabass::infrastructure;

int main()
{
    // A handful of edits never triggers the whole-file route, even
    // against a tiny file where the math would otherwise favor it.
    {
        BulkWriteStrategyInputs inputs;
        inputs.itemCount = 3;
        inputs.existingFileBytes = 1024;
        inputs.knownSequentialBytesPerSec = 50.0 * 1024 * 1024;
        assert(!shouldUseWholeFileReplace(inputs));
        std::cout << "case 1 (few items -> direct path regardless of size) OK\n";
    }

    // Many items, a small/fast-to-copy existing file, known-fast stick ->
    // whole-file route clearly wins.
    {
        BulkWriteStrategyInputs inputs;
        inputs.itemCount = 500;
        inputs.existingFileBytes = 2 * 1024 * 1024;
        inputs.knownSequentialBytesPerSec = 50.0 * 1024 * 1024;
        assert(shouldUseWholeFileReplace(inputs));
        std::cout << "case 2 (many items, small file, fast stick -> whole-file) OK\n";
    }

    // Many items, but the existing file is huge -> copying it (twice)
    // would take longer than just doing the direct writes.
    {
        BulkWriteStrategyInputs inputs;
        inputs.itemCount = 500;
        inputs.existingFileBytes = static_cast<std::uintmax_t>(20) * 1024 * 1024 * 1024;
        inputs.knownSequentialBytesPerSec = 5.0 * 1024 * 1024;
        assert(!shouldUseWholeFileReplace(inputs));
        std::cout << "case 3 (huge existing file -> direct path) OK\n";
    }

    // The minimum-item floor holds even when the size/throughput math
    // alone would say yes.
    {
        BulkWriteStrategyInputs inputs;
        inputs.itemCount = 5;
        inputs.existingFileBytes = 1024;
        inputs.knownSequentialBytesPerSec = 1.0;  // deliberately terrible
        assert(!shouldUseWholeFileReplace(inputs));
        std::cout << "case 4 (min-item floor holds regardless of math) OK\n";
    }

    // No known throughput -> falls back to the conservative default
    // rather than refusing outright; still finds the clear win here.
    {
        BulkWriteStrategyInputs inputs;
        inputs.itemCount = 1000;
        inputs.existingFileBytes = 1024;
        assert(shouldUseWholeFileReplace(inputs));
        std::cout << "case 5 (unknown throughput -> conservative fallback still decides) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
