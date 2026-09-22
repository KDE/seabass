<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Rules for code that writes a DJ's library

These are about production code, not tests. `docs/testing.md` covers how
to know a check is really checking; this covers how the writes themselves
should behave when they meet something they were not built for.

There is one rule so far. It earned its own file because three separate
sessions arrived at it independently in one week, each from a different
bug, and because the obvious alternative is wrong in a way that is hard
to see afterwards.

## Refuse, do not transliterate

**When a write path meets input outside what it was designed for, it must
fail loudly rather than do its best.**

The tempting answer is always to widen it: make the function handle the
other case properly. Sometimes that is right. But when the "best effort"
produces output that is *structurally valid and semantically wrong*, it
is the worst of the options, because nothing downstream can tell that
anything happened.

The live example, and the one this was written for.
`PdbRowWriter`'s `fitAsciiToCapacity()` fits replacement text into a
fixed-width `device_sql_string`. Its comment says "anonymized placeholder
text is always plain ASCII, so byte-level truncation/padding never splits
a multi-byte character". That is true of every caller today. Nothing
enforces it, and the UTF-16 branch writes each byte with a zero high
byte, so a non-ASCII name reaching it would be silently mangled into
valid-looking nonsense rather than rejected.

### Why "handle it properly" is the wrong instinct here

Because of what failure looks like in this format. Three separate
silent-corruption bugs in `src/infrastructure/rekordbox/` in one week,
all with the same signature:

| Bug | What it did | Why nothing noticed |
|---|---|---|
| `tag_row` keep-range two bytes short | Zeroed a live field in all 28 rows | Names read back, tests passed, file reparsed |
| Sync's OneLibrary mirror | Failed the save and rolled back a good write | Only visible if you synced a track with no mirror row |
| Clean Up's merged-cue mirror | Swallowed the failure, reported success | The deletion it guards then ran anyway |

In every case the file stayed parseable and every counter stayed green.
The damage was only visible byte by byte against the original. A write
path that quietly does its best is how each of those got as far as it
did.

Refusing is loud, and loud is repairable. A save that stops with a
message costs someone a minute; a save that writes plausible wrong bytes
into a library costs them the library, and they find out at a gig.

### The corollary, which is where this actually bites

**A comment saying "callers always pass X" is a note, not a guarantee.**
If the code would *corrupt* rather than *fail* on not-X, it needs a guard
while every caller still passes X. The distance between latent and live
is one new caller.

That is not theoretical for this function. `overwriteAllTagNames()` now
writes through it, and the values next to it in that file are **My Tag
names: free text a DJ typed**, in a format used across Europe. The
placeholders the anonymizer writes are ASCII by construction, so the
precondition still holds and the bug stays latent. It stays latent
exactly until one code path writes a real name instead of a placeholder.

### Applying it

- A write path that cannot represent its input **returns a failure**; it
  does not truncate to something representable, substitute a similar
  character, or write the bytes it can and drop the rest.
- Preconditions that would cause corruption get a runtime guard, not a
  comment, however reliably the callers currently honour them.
- A guard is worth having even when no caller can trip it today. State
  that in the guard, so the next person does not remove it as dead code.
