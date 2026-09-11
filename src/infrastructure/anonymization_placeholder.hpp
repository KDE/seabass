// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <functional>
#include <sstream>
#include <string>

namespace seabass::infrastructure
{

// A placeholder derived deterministically from `realKey` -- a real
// value worth obfuscating consistently -- rather than a per-run
// sequential counter: the same real key always produces the same
// placeholder, even across two completely independent anonymization
// runs over two different catalogs (rekordbox and Engine) describing
// the same real USB stick. That matters specifically for track
// filenames: domain::TrackMatcher's own primary cross-catalog matching
// signal is exactly this (its own comment: "100% of rekordbox tracks
// matched their Engine counterpart by path" -- the single most
// reliable signal it has). A per-catalog-run sequential counter would
// assign unrelated placeholder filenames to what's really the same
// track in each catalog, breaking that correlation and making any
// sync-matching test against anonymized data look broken even when the
// real matching logic is fine.
//
// realKey empty -> a fixed placeholder rather than hashing an empty
// string, since an empty key carries no real information to begin
// with.
//
// Hash comes before `kind` in the output (not "Track a3f9e2" but
// "a3f9e2 Track") deliberately: the rekordbox side's own
// PdbRowWriter::overwriteTrackText() preserves the original field's
// on-disk byte length and silently truncates whatever's written to
// fit, so a track whose real title/comment was only a handful of
// bytes long gets this placeholder cut short. With `kind` first, that
// truncation used to eat the entire differentiating hash digits and
// leave every short-titled track with the exact same "Trac"/"Track "
// prefix -- a real, observed collision across ~7.6% of a real 1,370-
// track library. Hash-first means truncation instead eats the
// human-readable label, which carries no uniqueness anyway. Not a
// hard guarantee for a pathologically short original field (under
// ~7 bytes, same caveat as anonymizationFilenamePlaceholder() below)
// -- real DJ library titles essentially never are.
inline std::string anonymizationPlaceholder(const std::string &kind, const std::string &realKey)
{
    if (realKey.empty()) {
        return kind + " (none)";
    }
    size_t h = std::hash<std::string>{}(realKey);
    std::ostringstream oss;
    oss << std::hex << (h & 0xFFFFFF) << " " << kind;
    return oss.str();
}

// Same idea as anonymizationPlaceholder() above, but deliberately as
// short as possible -- just a hex hash plus the real file's own
// extension, no "Kind " word prefix -- because *this* specific value is
// the one that actually has to survive PdbRowWriter::
// overwriteTrackText()'s byte-length-preserving field overwrite on the
// rekordbox side: text longer than the real filename it replaces gets
// silently truncated to fit, which would break the exact cross-catalog
// correlation anonymizationPlaceholder()'s own comment explains this
// exists for. Engine's obfuscated filename has no such length
// constraint, so keeping this short is specifically about giving the
// rekordbox side its best chance of not truncating away the difference.
// Not a hard guarantee for a pathologically short real filename (under
// ~7 bytes) -- real DJ library filenames essentially never are.
inline std::string anonymizationFilenamePlaceholder(const std::string &realFilename)
{
    if (realFilename.empty()) {
        return "unknown.mp3";
    }
    size_t h = std::hash<std::string>{}(realFilename);
    std::ostringstream oss;
    oss << std::hex << (h & 0xFFFFFF);
    size_t dot = realFilename.rfind('.');
    std::string ext = dot != std::string::npos ? realFilename.substr(dot) : ".mp3";
    return oss.str() + ext;
}

}  // namespace seabass::infrastructure
