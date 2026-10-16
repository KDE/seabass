#version 440
// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Cover art with rounded corners (ArtworkImage): the Image itself is the
// texture, so no item renders to a layer of its own, and a list of
// hundreds of rows draws hundreds of quads, not hundreds of offscreen
// buffers. Outside the rounded rectangle is transparent, with a one
// pixel soft edge so the corner is not stair-stepped.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 itemSize;   // the item, in pixels
    float radius;    // the corners' radius, in pixels
    vec2 texScale;   // the item's share of the painted image on each axis
};

layout(binding = 1) uniform sampler2D art;

void main()
{
    // The painted image may be larger than the item (cropped to fill it)
    // or smaller (fitted inside): the texture spans the painted image,
    // centred on the item.
    vec2 uv = 0.5 + (qt_TexCoord0 - 0.5) * texScale;
    // The corners are the picture's: the item's when it is cropped to fill
    // it, the fitted picture's when it is smaller.
    vec2 p = (qt_TexCoord0 - 0.5) * itemSize;
    vec2 halfSize = min(itemSize, itemSize / max(texScale, vec2(1e-6))) * 0.5;
    float r = min(radius, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - (halfSize - vec2(r));
    float outside = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    float coverage = clamp(0.5 - outside, 0.0, 1.0);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
        coverage = 0.0;
    }
    fragColor = texture(art, uv) * coverage * qt_Opacity;
}
