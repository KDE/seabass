<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Housekeeping

The Housekeeping hub (a stick's Maintain group, "Housekeeping") gathers
the tools that tidy a stick up. Each destructive one is a page of its own,
so nothing irreversible sits one stray click from a read-only view.

- **Match Duplicate Cues**: every copy of a track gets the same cues, and
  the page shows how much space the copies take.
- **Clean Up Duplicates**: removes redundant copies from the libraries and
  keeps the best one. The audio files it orphans are not deleted; they
  are listed for the next tool.
- **Delete Orphaned Files**: deletes the audio files earlier cleanups took
  out of every library, after checking again that nothing uses them.
- **Clean Up Stray Cues**: removes cues sitting at 0:00.
- **Clean Up Recordings**: deletes set recordings the players left on the stick, below.

## Clean Up Recordings

DJ hardware records sets onto the stick you play from:

| Folder on the stick | Written by | Files |
|---|---|---|
| `Sessions/` | Engine OS players (Prime 4, Prime Go+, SC series) | `Session-0001.wav`, or the name you gave it |
| `PIONEER REC/` | Pioneer DJ gear (OMNIS-DUO manual; XDJ-RX2 reported) | `REC001.WAV`, 44.1 kHz 16 bit |
| `PIONEER DJ REC/` | the XDJ-RX2 according to one summary of its manual (not yet seen on a stick) | `REC***.WAV` |
| `ALPHATHETA REC/` | AlphaTheta gear (an empty one was found on a stick the OMNIS-DUO had used) | `REC***.WAV` |

Folder names are compared without regard to case. These recordings are
large (a two hour set is about 1.3 GB), none of the three libraries refers
to them, and by the time a stick fills up they have usually long been
copied off and reworked elsewhere. So this tool deletes them and copies
nothing first.

The card says what is there: "3 recordings, 5.8 GiB, from Engine OS and a
Pioneer deck", or "No recordings on this stick", in which case it is off.
When a recording folder is there but cannot be read, it says "Could not
read the recording folders on this stick" instead, and is off too: a
folder it could not read is not reported as empty.

The page lists each recording with its folder, date, size and length, each
with its own checkbox. Nothing is ticked until you tick it (or use Select
All), and the bar at the top shows what the ticked ones would free on the
stick, the same bar Clean Up Duplicates shows.

**What is deleted**: only audio files (wav, aif, aiff, mp3, flac, m4a)
directly inside those folders, and only the ones you ticked.

**What is left alone**: anything else in those folders (named on the page),
subfolders, links, hidden files such as macOS's `._` files, and the folders
themselves (the hardware makes them again anyway). `Contents/`, `PIONEER/`,
`Engine Library/` and `Seabass/` are never looked at.

Each row has a Play button, the one Library Health's rows have: the
recording plays in the player bar as a plain file, named after its folder
and deck, with the play key in that deck's form. If it is playing when it
is deleted, the player lets go of it first.

**Delete Selected Recordings** asks first, naming how many and how much space they
free, and saying they will be gone for good. Nothing is copied and no
backup holds them: they belong to no library, so a stick backup of the
libraries would not either, and nothing is added to the orphaned files
list. Cancel finishes the file in flight and stops. A recording that is
gone when its turn comes (the stick was pulled) is reported as not
deleted, never counted as done, and every deletion is checked against the
stick afterwards. Each one is written to the stick's own log
(`Seabass/seabass.log`).
