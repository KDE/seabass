// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Sampling a grabbed image at LOGICAL coordinates.
//
// grabImage() returns DEVICE pixels; everything a test knows about
// geometry -- an item's width, mapToItem(), a corner three pixels in --
// is logical. On a display at devicePixelRatio 2 the grab of a 44x44
// item comes back 88x88, so a test reading pixel(22, 22) for "the
// middle" reads the top-left quadrant instead, and one reading a corner
// finds whatever is a quarter of the way in.
//
// That is not hypothetical and it does not fail loudly. It cost this
// project an afternoon on macOS: a row of a popup reported as painting
// nothing while the screenshot of the same run showed it drawn plainly,
// because its band had been sampled across the neighbouring row's
// boundary. The suite then agreed with the wrong conclusion three
// different ways before anyone compared the table with the picture.
//
// So no test works out the ratio for itself. It comes from the image
// that came back, measured against the item that was grabbed, which is
// the only honest source for it.
.pragma library

function scale(image, grabbedItem) {
    return image.width / grabbedItem.width;
}

// The pixel at a logical point inside the grabbed item. Clamped, because
// an assertion about the last column asks for width - 1 and the scaled
// answer must still be inside the image.
function pixel(image, grabbedItem, x, y) {
    var s = scale(image, grabbedItem);
    var px = Math.min(image.width - 1, Math.max(0, Math.round(x * s)));
    var py = Math.min(image.height - 1, Math.max(0, Math.round(y * s)));
    return image.pixel(px, py);
}
