<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
-->

# rekordbox XML export

`seabass-cli export-xml` writes a rekordbox collection XML — the `DJ_PLAYLISTS`
document rekordbox imports under *Preferences → View → Layout → rekordbox xml*.

## Why this exists

Every other format Seabass speaks describes a USB stick: `export.pdb`,
`exportLibrary.db`, Engine's `m.db`. rekordbox on a computer imports **none** of
them into its own library. XML is the documented door, and it is the only one.

That matters because of where the cues actually are. Measured on the WHALESHARK
stick:

| | rekordbox catalog | Engine catalog |
|---|---|---|
| tracks | 1471 | 1658 |
| cues | 110 | 1781 |

A DJ who cues on Denon gear has their work in the Engine database. Exporting
rekordbox's own view of the stick would carry 110 cues and silently leave the
rest behind. Merging the catalogs first is the whole point of the feature.

## What it does, in order

1. **Read** each catalog given (`KaitaiRekordboxReader`, `LibdjinteropEngineReader`).
2. **Collapse rows into files** (`application::collapseCatalogRows`). Three
   catalogs listing one file is the library being *correct*, not duplication;
   collapsing is also what unions their cues, and it is the step that carries
   Engine's cues to the rekordbox side. On the real stick: 3129 rows → 1471 files.
3. **Drop junk memory cues** (`domain::isJunkMemoryCue`), unless `--keep-junk-cues`.
4. **Filter** streaming rows (their path names a cache on another machine),
   rows with no resolvable path (rekordbox keys its collection on `Location`),
   and any extension named by `--exclude-ext`.
5. **Rewrite paths** by longest-matching `--map FROM=TO` prefix.
6. **Write** the document (`infrastructure::rekordbox::writeRekordboxXml`).

## Decisions that are not obvious

**A hot cue slot holds exactly one cue.** Where two catalogs claim the same slot
at different positions, the merge cannot keep both: the first catalog in the list
wins and the other position is dropped (`domain::LocalRestorePlanner::mergeCues`).
`--prefer engine` puts Engine first. Every such case is reported as a conflict,
because it is the only record that something was discarded. Positions within
100 ms are cross-format rounding, not disagreement.

**Junk cues are most of what a stick holds.** 1448 of the WHALESHARK stick's
1594 memory cues were Engine's own auto-placed `main_cue`, leaving 146 real cues
on 47 tracks — 36 of which rekordbox did not have. Reporting the pre-cleanup
number as "cues recovered from Denon" would overstate the result by 40×.

**A memory cue at a negative position is junk.** Engine rounds `main_cue` a hair
below the first sample: 1388 of those 1448 read back at `-0.000 ms`. The rule
used to require `positionMs >= 0.0`, which let every one of them past the check
written to catch exactly that cue. Hot cues are excluded by *kind* at any
position, so a deliberate "track start" pad still survives. Cue positions are
additionally clamped at zero on write — `Start="-0.000"` is not seekable.

**Tags contain bytes XML cannot hold.** A single one makes the document
unparseable and rekordbox rejects the file without saying which of 1471 tracks
did it. Found on this stick: artist `A�\x0C`, a form feed inside an ID3
frame. `escapeXmlText` drops the control characters XML 1.0 forbids and replaces
malformed UTF-8 with U+FFFD. Tab/LF/CR become character references so attribute
normalization cannot turn them into spaces.

**Ratings are stars × 51.** rekordbox stores 0/51/…/255. Writing 0–5 imports as
"very nearly unrated" on every track and looks like the ratings were lost.

**FLAC is excluded by request, not by capability.** rekordbox reads FLAC and will
happily export it to a stick — an XDJ-RX2 then cannot play it. `--exclude-ext
flac` builds a player-safe collection without moving a file.

## Format notes (as observed)

```xml
<DJ_PLAYLISTS Version="1.0.0">
  <PRODUCT Name="Seabass" Version="0.1" Company="Seabass"/>
  <COLLECTION Entries="1471">
    <TRACK TrackID="1" Name="…" Artist="…" Album="…" Kind="MP3 File" Size="…"
           TotalTime="349" AverageBpm="128.00" BitRate="320" Tonality="Fm"
           Rating="204" PlayCount="2" Location="file://localhost/…">
      <TEMPO Inizio="0.000" Bpm="128.00" Metro="4/4" Battito="1"/>
      <POSITION_MARK Name="" Type="0" Start="30.765" Num="1" Red="255" Green="0" Blue="23"/>
    </TRACK>
  </COLLECTION>
  <PLAYLISTS>
    <NODE Name="ROOT" Type="0" Count="2">…</NODE>
  </PLAYLISTS>
</DJ_PLAYLISTS>
```

- `Num="-1"` is a memory cue; `0`–`7` are hot cue slots A–H. Writing `0` for a
  memory cue silently makes it hot cue A.
- `Type="0"` is a cue, `Type="4"` a loop (which also carries `End`).
- `NODE Type="0"` is a folder, `Type="1"` a playlist; `KeyType="0"` means
  `<TRACK Key="…"/>` refers to `TrackID`. The outermost node must be a folder
  named `ROOT`, empty or not, or rekordbox shows no playlists at all.
- rekordbox has no node that is both folder and playlist. A path used as both
  becomes a folder holding a same-named playlist.
- `Location` is `file://localhost` + percent-encoded UTF-8, `/` kept as the
  separator. Byte-exact encoding is what preserves an NFD-decomposed macOS
  filename.
- `TrackID` is assigned here, 1-based. Catalog source ids collide across formats
  (rekordbox row 19 and Engine row 19 are different tracks) and a collapsed file
  carries several at once.

Output is byte-identical across runs over the same library, which is what makes
it diffable and the tests assertable.

## What does not survive

- **Beatgrids.** Seabass reads them but has never written them
  (`anlz_file.hpp` copies every non-cue section through verbatim), and `TEMPO`
  carries only a first beat. rekordbox's own analysis fills the rest in.
- **Waveforms and artwork.** Not part of this document; rekordbox regenerates
  waveforms on analysis and reads artwork from the file's own tags.
- **My Tag / colour labels**, comments beyond the free-text field, and anything
  else `exportExt.pdb` holds.

## v2

This is deliberately a CLI-and-use-case slice, sized to be the base of the real
feature rather than the feature itself. Left for v2:

- A GUI page over `ExportRekordboxXml`, showing the conflict list before writing
  and letting a per-conflict winner be chosen rather than one global `--prefer`.
- Reading the XML back, so a collection edited in rekordbox can be compared with
  what is on a stick (`DJ_PLAYLISTS` is a documented import *and* export format).
- Writing a first-beat `Inizio` from Engine's stored beatgrid, which is read
  today and thrown away here.
- Folding `tools/library-build/` (see `library-rebuild.md`) into use cases, so
  reorganising files on disk and emitting the XML that points at them is one
  operation rather than a script plus a command.
