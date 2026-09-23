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
fail loudly rather than do its best.** And -- the half that is easy to
lose -- it must not mistake input it *can* handle for input it cannot.
Both directions are in the table below, because both happened this week.

The tempting answer is always to widen it: make the function handle the
other case properly. Sometimes that is right. But when the "best effort"
produces output that is *structurally valid and semantically wrong*, it
is the worst of the options, because nothing downstream can tell that
anything happened.

The live example, and the one this was written for.
`PdbRowWriter`'s `fitAsciiToCapacity()` fits replacement text into a
fixed-width `device_sql_string`. Its comment says "anonymized placeholder
text is always plain ASCII, so byte-level truncation/padding never splits
a multi-byte character". That is true of every caller today, and nothing
enforces it.

Measured rather than predicted, because the first draft of this section
said "would be mangled" and that was a guess. Writing `Cé` into a
`device_sql_long_utf16le` field stores `CÃ`:

```
in:     C    é           43 c3 a9   (UTF-8, 3 bytes)
stored: C    Ã           43 c3 83   (2 UTF-16 code units)
result: overwriteTrackText() returns TRUE
```

Two separate things go wrong and neither is visible afterwards. The
UTF-16 branch writes each *byte* of the UTF-8 input as the low half of a
code unit with a zero high byte, so `é` (`c3 a9`) becomes `Ã` plus a
second character; and capacity is counted in code units while the input
is counted in bytes, so the tail is silently dropped. Every length is
right, the field reparses, the call reports success, and one character of
a DJ's text is gone.

### Why "handle it properly" is the wrong instinct here

Because of what failure looks like here. Four write-path bugs in one
week, every one of them a path meeting input outside what it was built
for -- and each giving a *different* wrong answer:

| Bug | Where | Input it was not built for | Wrong answer |
|---|---|---|---|
| `tag_row` keep-range two bytes short | `infrastructure/rekordbox/pdb_row_writer.cpp` | A row shape with a field it did not know about | **Corrupted silently.** Zeroed a live field in all 28 rows; names still read back, tests passed, file reparsed |
| `fitAsciiToCapacity` UTF-16 branch | same file | Non-ASCII text | **Corrupted silently.** Stores `CÃ` for `Cé` and returns true; every length right, field reparses |
| Sync's OneLibrary mirror | `gui/edit/changes/sync_plan_change.cpp` | A track with no Device Library Plus row | **Refused wrongly.** Failed the save and rolled back a good write, for 635 of 1118 tracks |
| Clean Up's merged-cue mirror | `gui/edit/changes/cleanup_group_change.cpp` | A mirror write that failed | **Succeeded falsely.** Logged it and reported success -- and Clean Up then deletes the duplicates those cues were merged from, so the merged set is the only copy and half of it is missing |

Two things that set is worth noticing for.

**The failure follows the kind of code, not the module.** Two are deep in
the binary format layer and two are in GUI change classes. What they have
in common is that each is a *write path*, not that they live near each
other.

**And "refuse" is only half the rule.** One of the four is a refusal that
was itself the bug. Sync met a perfectly ordinary situation -- a track
with no mirror row, which is most tracks on a real stick -- and treated
it as a failure. So:

> Refuse what you cannot represent. Do not refuse what you can handle,
> and do not report success for what you did not do.

The three wrong answers are the three ways out of that, and only one of
them is right for any given input. What makes silent corruption the worst
of them is not that it is the most likely, but that it is the only one
nothing downstream can detect: a wrong refusal is a message on screen,
and a false success is at least visible in a log. Corrupted bytes in a
DJ's library are found at a gig, months later, by the person who needed
them.

### "Cannot represent" is not "cannot fit"

The rule refuses what the field **cannot represent**. It does not refuse
what the field represents by a documented lossy contract, and the two
look alike enough that a careful reader has already mistaken one for the
other.

`PdbRowWriter` cannot resize a row: a pdb row that grew would have to
reflow its page, so every overwrite preserves the field's on-disk byte
length and text longer than the span is **truncated to fit**. That is
the class's stated contract, not an accident, and the anonymizer -- the
only production caller -- depends on it. `anonymizationPlaceholder()`
puts its hash *before* the human-readable word precisely because
truncation eats the tail, and a short field would otherwise leave every
track with the same `Trac` prefix.

So "make truncation refuse, the caller is not being told" would break
anonymisation on every short field, in the name of this document.

The distinction to apply:

| | |
|---|---|
| **Cannot represent** | The write would produce *different* content: non-ASCII through a byte-per-code-unit path, a value the field has no encoding for. **Refuse.** |
| **Cannot fit** | The write produces *less* content, by a contract the caller chose and the format forces. **Do it, and make sure the loss is visible somewhere.** |

For truncation, "visible somewhere" is the open end. Nothing counts how
often a placeholder was cut short, so a donated export cannot say how
many of its fields were too small to carry a full one. That is a
reporting gap worth closing; it is not a reason to start refusing.

And the latent case is the same shape as every other one here: the
contract holds because every caller today is an anonymiser writing
placeholders it does not mind losing the tail of. The first caller that
writes text a person typed and expects it back intact is the moment
truncation becomes the wrong answer -- and nothing would announce that
either.

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
