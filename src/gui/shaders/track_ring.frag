#version 440
// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A whole track as a ring around its cover art: 12 o'clock is the start,
// clockwise is time, a bar's length is that moment's energy. Everything
// is read from two textures -- the waveform as an N x 1 strip (r, g, b =
// low, mid, high) and the art -- so a frame costs the CPU a few uniforms.
//
// What moves, while a track plays: the art swells and throws a halo with
// the bass; the bars under the playhead jump with it; a slow wave of
// light travels round the played part, as strong as the mids; the white
// roots flare with the highs; and every beat sends a ripple out from the
// art through the bars.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float progress;    // 0..1 of the track played; below 0 for "not playing"
    float bass;        // how loud the low band is right now, 0..1
    float mid;         // and the mid band
    float high;        // and the high band
    float time;        // seconds, running while the track plays
    float rippleAge;   // seconds since the last beat; large for "none"
    float rippleSpan;  // seconds a ripple takes to cross the bars
    float rippleStrength;
    float artTurns;    // how far round the cover has spun, in turns, 0..1
    float bars;        // how many bars go round
    float artRadius;   // the art disc, in units of the ring's outer radius
    float hasArt;
    vec4 fallbackColor;
    vec4 backgroundColor;   // the page behind: what "dimmed" fades toward
};

layout(binding = 1) uniform sampler2D wave;
layout(binding = 2) uniform sampler2D art;

const float TAU = 6.28318530718;

// The art, heavily blurred, looked up in the direction this bar points:
// the cover bleeds outward into its own ring.
vec3 ringColour(vec2 dir)
{
    if (hasArt < 0.5)
        return fallbackColor.rgb;
    vec3 c = vec3(0.0);
    // Nine wide taps stand in for a blur; the art has no mipmaps to lean on.
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            vec2 jitter = (vec2(float(i), float(j)) - 1.0) * 0.09;
            c += texture(art, clamp(vec2(0.5) + dir * 0.30 + jitter, 0.02, 0.98)).rgb;
        }
    c /= 9.0;
    // Push it away from grey and up to a brightness that reads on a dark
    // page; a black-and-white cover still gives a black-and-white ring.
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    c = clamp(mix(vec3(luma), c, 1.7), 0.0, 1.0);
    float peak = max(max(c.r, c.g), max(c.b, 0.001));
    return c * (clamp(peak, 0.75, 1.0) / peak);
}

void main()
{
    vec2 p = (qt_TexCoord0 - 0.5) * 2.0;
    float r = length(p);
    float px = fwidth(r);
    vec2 dir = p / max(r, 0.0001);
    float t = fract(atan(p.x, -p.y) / TAU + 1.0);

    // This bar: the loudest of the columns it spans, so a short peak
    // between two sample points is not lost.
    float k = t * bars;
    float bar = floor(k);
    float f = fract(k);
    vec3 w = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        float u = (bar + (float(i) + 0.5) / 4.0) / bars;
        w = max(w, texture(wave, vec2(u, 0.5)).rgb);
    }

    float discRadius = artRadius * (1.0 + 0.05 * bass);
    float r0 = artRadius + 0.07;
    float room = 1.0 - r0;
    // The bars under the playhead jump with the bass: that is where the
    // music is. (t - progress does not wrap at twelve o'clock; the first
    // and last second of a track can live without the jump.)
    float ahead = t - progress;
    float here = progress >= 0.0 ? exp(-abs(ahead) * 38.0) : 0.0;
    float jump = 1.0 + here * (0.08 + 0.50 * bass);
    float lowReach = min(max(w.r, 0.03) * jump, 1.0) * room;
    float midReach = min(w.g * 0.72 * jump, 1.0) * room;
    float highReach = min(w.b * 0.45 * jump, 1.0) * room;
    float reach = max(lowReach, max(midReach, highReach));

    vec3 c = ringColour(dir);
    // Fainter means nearer the page's own colour, not nearer black: the
    // page can be a light one.
    vec3 page = backgroundColor.rgb;
    vec3 colour = mix(page, c, 0.50);
    colour = mix(colour, c, 1.0 - smoothstep(midReach - px, midReach + px, r - r0));
    colour = mix(colour, mix(c, vec3(1.0), 0.65 + 0.35 * high), 1.0 - smoothstep(highReach - px, highReach + px, r - r0));

    // A pixel's width measured in bars, worked out rather than taken from
    // fwidth(k): k jumps from `bars` to 0 at 12 o'clock and fwidth would
    // draw a seam there.
    float kpx = px / max(r, 0.001) * bars / TAU;
    float inBar = smoothstep(0.16 - kpx, 0.16 + kpx, f) * (1.0 - smoothstep(0.84 - kpx, 0.84 + kpx, f));
    float alpha = inBar * smoothstep(r0 - px, r0 + px, r) * (1.0 - smoothstep(reach - px, reach + px, r - r0));

    float played = 1.0;
    if (progress >= 0.0) {
        // What is still to come is dimmed; what was just played glows,
        // fading back round the ring like a comet's tail.
        played = 1.0 - smoothstep(-0.5 / bars, 0.5 / bars, ahead);
        float luma = dot(colour, vec3(0.299, 0.587, 0.114));
        colour = mix(mix(page, mix(vec3(luma), colour, 0.6), 0.32), colour, played);
        float tail = played * exp(ahead * 45.0);
        colour += c * tail * 0.55;
    }

    // A slow wave of light going round what has been played, as strong
    // as the mids are loud: motion between the beats.
    float wave = 0.5 + 0.5 * sin(TAU * (t * 14.0 - time * 0.30));
    colour *= 1.0 + 0.35 * mid * wave * played;

    // A beat sends a ripple out from the art, crossing the bars in
    // rippleSpan -- most of a beat, so it is gone as the next one starts
    // -- and fading as it goes.
    float crossed = rippleAge / max(rippleSpan, 0.01);
    float front = r0 + crossed * room;
    float offFront = (r - front) / 0.045;
    float ripple = exp(-offFront * offFront) * exp(-crossed * 2.2) * step(crossed, 1.15) * step(r0, r) * rippleStrength;
    colour += c * ripple * 0.9;

    // The ring's base line, so silence still draws a circle.
    float base = (1.0 - smoothstep(0.0, 1.5 * px, abs(r - (r0 - 0.012)))) * 0.45;
    vec4 outColour = vec4(colour * alpha, alpha);
    outColour = outColour + vec4(c * base, base) * (1.0 - outColour.a);

    if (progress >= 0.0) {
        float arc = abs(ahead) * TAU * r;
        float onRing = smoothstep(r0 - 0.03 - px, r0 - 0.03 + px, r) * (1.0 - smoothstep(1.0 - px, 1.0, r));
        float line = (1.0 - smoothstep(0.004, 0.004 + px, arc)) * onRing;
        float glow = exp(-arc * 55.0) * 0.35 * onRing;
        outColour = mix(outColour, vec4(1.0), line);
        outColour += vec4(c * glow, glow) * (1.0 - outColour.a);
    }

    // Between the bars and past their tips the ripple still shows, faintly.
    float faint = ripple * 0.14 * (1.0 - smoothstep(1.0 - px, 1.0, r));
    outColour += vec4(c * faint, faint) * (1.0 - outColour.a);

    // The bass lifts a halo off the art's edge.
    float halo = bass * 0.55 * exp(-max(r - discRadius, 0.0) * 28.0) * step(discRadius, r);
    outColour += vec4(c * halo, halo) * (1.0 - outColour.a);

    // The art itself, as a disc that swells a touch with the bass.
    float disc = 1.0 - smoothstep(discRadius - px, discRadius + px, r);
    // It spins like the record it is the sleeve of: clockwise, which with
    // y pointing down means looking the picture up turned back the other
    // way. Only the cover turns -- the ring's colours stay where they are,
    // because a place on the ring is a moment in the track.
    float spin = artTurns * TAU;
    vec2 onCover = vec2(cos(spin) * p.x + sin(spin) * p.y, -sin(spin) * p.x + cos(spin) * p.y);
    vec3 artColour = hasArt > 0.5
        ? texture(art, clamp(vec2(0.5) + onCover / (discRadius * 2.0), 0.0, 1.0)).rgb
        : mix(page, fallbackColor.rgb, 0.18);
    outColour = mix(outColour, vec4(artColour, 1.0), disc);

    fragColor = outColour * qt_Opacity;
}
