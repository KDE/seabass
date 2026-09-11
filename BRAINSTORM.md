<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Ideas for features

Here are some ideas which could be implememted in Seabass. No guarantees, this is NOT a roadmap, just a record of brainstorming.

## Loops

Show / display loops on tracks

## Better device settings

- add (i) for each setting: it's not always clear what it does
- nicer headings
- on / off gets switches instead of dropdowns
- settings backup and restore
- deck colors?
- cue point colors?

## DBus interface for creating screenshots

- behind a cli switch
- makes updating the website easier

## Developer Settings category:

- Prune/obfuscate my own usb stick data and add it as test dataset, create a testsuite which tests our functionality against these datasets

- Allows submitting usb stick libraries to upstream developer to add to testing harness, after obfuscation (track titles, artist names, etc.); tell transparently what data is submitted, and that no personal data or useful tracks/playlist are being submitted, it helps making seabass work with YOUR USB stick though, for very little effort. (Mention that we do not EVER phone home without explicit permission, and that privacy is a core feature. Recipient is sebas@kde.org)

- Add version, show it in --help and on About page, current version is 0.5 (beta)
- Add "Seabass is beta software. While we take utmost care with your data, operation is with inherent risks. Seabass creates backups, but you do use it at your own risk. Make sure you operate on backed-up data. Also: we strongly advice to test your stick on target hardware before your gigs and to always carry a backup."
- Developer credits


## Experimental features

- Switch in settings for experimental features (plus warning), off by default, claude should consider new features as experimental and keep track, suggest moving to "stable", build flag for experimental stuff

## USB Stick statistics:
    - stick filesystem (indicate which devices are supported with this FS, and where the limitations are (e.g. vfat, may not work on XDJ-RX or somesuch, max file size, etc.)
    - number of tracks and playlists, cue points etc. (per library type), number of tracks per key, number of track with rating, comments, BPM distribution, distribution of formats, tracks from streaming services including which one, ...
    - Used/free space plus nice diagram of where the diskspace goes (metadata per library, coverart, files, may dig down into files per playlist / artist, bit like KDE's filelight)
    - Speed measurements: reading speed, invent a score for read-speed (tracks / raw data and db, and save it locally per stick so we can compare stick performance at least for this laptop); make sure to note which USB protocol is used (USB-A or -C usually and keep stats for both)
    
## Adding metadata

- genre(s) from online sources
- which additional metadata can we find?

## Removing tracks

- Some tracks are just wrong, want easy way to clean this up
    
## Playlist creation / editing / removal

- Adding tracks to playlist from library view, including "New playlist, should remember last chosen one"
- Reordering playlists
- Editing playlist metadata
- Removal (with and without orphaned track deletion (i.e. tracks not in another list))
- Show / filter on tracks not currently in any playlist
- Filter on tracks with same key, within key range, within BPM range of N%, genre

## Transcode playlist

- Playlist as FLAC or other format with limited support: transcode to mp3@320 or somesuch so it can play on older hardware (create new playlist, sync cues and metadata), recognize these playlist in cleanup and duplicate, allow cleaning up but disable this by default

## Formatting USB sticks

Allow formatting (with all the usual warnings and safeguards); pick format based on hardware support, also allow adding artwork to a stick to easily recognize.

## Full stick backups (incl data) 

- Creating backups of a stick (either to / from local disk or directly from one USB device to another)

## KDE Readiness

Where the repo lives now -- `invent.kde.org/sebas/seabass`, a personal namespace -- almost none of this is required: personal repos only have to be KDE-related and follow the Code of Conduct. The list below is what would have to be true before the repo could move into the `kde/` namespace as a real KDE project (incubation, then a sysadmin move).

- REUSE compliance. The big one, and repo-wide: KDE's licensing policy mandates REUSE 3.0, i.e. every file carries copyright plus an `SPDX-License-Identifier` tag, and a `LICENSES/` directory holds the text of every license used. Today there are zero SPDX tags across ~4700 tracked files and only a root `LICENSE` with GPL-2 text. `reuse annotate` can do most of the mechanical work; the judgement calls are the third-party and generated files.
- License choice. GPL-2.0-only is on KDE's allowed list for applications, but the expected form for a new app is the disjunction `GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL`, which is what lets KDE e.V. accept future GPL versions on the project's behalf.
- CI. There is no `.kde-ci.yml`, and `.gitlab-ci.yml` is GitLab's stock SAST/Secret-Detection templates. KDE asks projects not to hand-roll CI and to include `sysadmin/ci-utilities` templates instead (`/gitlab-templates/reuse-lint.yml`, `/gitlab-templates/linux.yml`, ...), with dependencies and code-quality options declared in `.kde-ci.yml`.
- Submodules. `third_party/` pulls kaitai_struct_cpp_stl_runtime and libdjinterop as gitlinks from GitHub. KDE CI does not build git submodules; dependencies are expected to come through repo-metadata (or be vendored outright). Related: Invent's commit audit rejects any commit containing a file named `.gitmodules`, which is why the file is generated from `cmake/dependency-submodules.txt` rather than committed -- see `scripts/init-submodules.sh`.
- AppStream. No `.metainfo.xml` and no `.desktop` file. Both are required for a releasable KDE application, and the metainfo file is what puts it in the software centres.
- repo-metadata. A move into `kde/` needs an entry in `sysadmin/repo-metadata` (projectpath, kind, lifecycle). That is filed as part of the move, not something the repo carries itself.
