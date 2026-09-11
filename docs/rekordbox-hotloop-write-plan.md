# Reverse-engineering plan: writing hot loops to rekordbox's ANLZ format

> **Status 2026-09-11.** Two things changed since this plan was written.
> First, every rewrite of a track's cue lists now carries the entries it
> did not change back **byte for byte** (`RawHotCueEntry::rawBytes`,
> `AnlzCueCodec::encodeHotCues`): an existing loop, comment or legacy
> colour id survives an Add Cue, Sync, Clean Up or Restore on that track,
> which it did not before -- every save flattened them silently. Second,
> a loop that arrives from the domain (an Engine hot loop synced over)
> is encoded as the spec's loop entry type with its out point in
> `loop_time`, the same two fields the reader takes it from. The
> validation work below, against captured real loop entries, still
> stands as the bar for calling loop *creation* confirmed; nothing here
> writes a comment yet.


## Why this exists

Seabass now *reads* loops from rekordbox correctly (`kaitai_rekordbox_reader.cpp`,
landed `9c20aa5`) — that's real kaitai-parsed data, high confidence. Writing a
*new* loop to rekordbox is deliberately not implemented: `AnlzCueCodec`'s own
doc comment already flags loop encoding as "v1 scope: ... no loop support",
with several raw byte fields marked LOW confidence or never tested at all,
because every real example captured so far (7 hot-cue entries) was a plain
point cue. `AddCueController` refuses a rekordbox loop write outright rather
than guess at those bytes — writing wrong bytes into a DJ's real library
risks corrupting cue data before a gig.

This plan gets loop writing to the same confidence bar the hot-cue writer
already cleared, using the same snapshot-diff methodology already used
throughout this project (cue color normalization, the ratings/comments
prep, etc.): never guess, always capture a real before/after and diff it.

## What we already know (free — no user action needed)

Decoded every field of the one real loop entry already in this repo's test
data (`testdata/PIONEER-rb7-stick/USBANLZ/P017/0002435D/ANLZ0000.EXT`, a
**memory-list** loop, hot_cue=0, 52583–56333ms):

| field | value | notes |
|---|---|---|
| pad3 (after `type`) | `00 03 E8` | matches the existing hot-cue "always 0x0003E8" finding — now confirmed for a loop entry too |
| loop_numerator/denominator | 8 / 1 | an 8-beat loop; 56333-52583=3750ms for 8 beats implies ~128 BPM — internally plausible, first real confirmation these fields mean what the spec says |
| pad7 (after color_id) | `01 02 46 02 02 00 00` | byte 0 matches the known-constant 0x01; bytes 1-6 are a single new data point, not enough to say whether the "unexplained variation" already seen in hot-cue examples has the same shape here |
| color / comment | none set | no new evidence either way for colored/commented loops |

This is one data point, on a **memory-list** loop — genuinely useful as a
prior, but the actual "hot loop" feature (hot_cue != 0, type == loop) has
**zero** real examples anywhere in this project.

## What's still unverified

- **Hot loops specifically** (hot_cue != 0 AND type == loop) — never seen one. Everything captured is either hot+point or memory+loop, never both flags at once.
- **pad7's real meaning** — the existing hot-cue doc comment's best guess is "an undocumented per-cue ordering/sequence field"; only 1 loop example exists to check that theory against.
- **loop_numerator/denominator for a *free* (unquantized) loop** — spec says "zero if not quantized," never observed.
- **Colored/commented loops** — untested.
- **Editing an existing loop's length** — does rekordbox rewrite `time`/`loop_time` in place, or touch the pad7 bytes too?
- **Deleting a loop** — does the entry disappear from the PCO2 list cleanly (like a deleted hot cue does), or does something else happen (the plain, non-extended `cue_entry.status` field is documented as "indicates if this is an active loop" — worth checking whether an equivalent "disabled" state exists somewhere in the extended entry's pad7 bytes instead of an outright removal)?
- **Hot-loop vs memory-loop byte layout** — assumed identical per the kaitai spec (both use `cue_extended_entry_t`), but only independently confirmed for hot *cues* and one memory *loop*, never a hot *loop*.
- **rekordbox desktop vs. real DJ hardware** — every capture so far, and every capture in the steps below, comes from rekordbox desktop software writing the export. Real CDJ/XDJ hardware sets/edits cues directly against a stick's `export.pdb`/ANLZ files too (rekordbox re-imports and re-exports afterward, but the hardware's own on-the-fly write may not be byte-identical to what desktop rekordbox would produce for the same cue) — this codebase has already run into a hardware-vs-desktop divergence once before (`libdjinterop_engine_reader.cpp`'s `sample_rate()` flakiness note on tracks re-cued on real Denon hardware). Worth cross-checking against a real XDJ-RX2 run, not just desktop software.

## Capture steps (needs your hands, real rekordbox desktop software, one throwaway test track)

Pick one track with no existing cues, so every capture stays clean. Before
each step: copy its `ANLZ0000.EXT` file somewhere safe. After each step:
copy it again. Send me both, plus a one-line note of exactly what you did
(slot number, quantized/free, color, comment, position) — I'll do the
byte-level diff and field attribution.

1. **Create one hot loop** (any slot, quantized/beat-locked length). The single most important capture — the one thing never seen before.
2. **Create a second hot loop, free length** (not beat-locked). Tests whether loop_numerator/denominator really go to 0/0, and whether pad7 changes with quantization state.
3. **Color one of the two hot loops**, same as you'd color a hot cue.
4. **Add a comment to one hot loop.**
5. **Edit an existing hot loop's length** (e.g. double it) without deleting it.
6. **Delete a hot loop.**
7. **Create a memory-list (non-hot) loop for comparison**, so hot-loop vs memory-loop layout can be checked directly against each other rather than against a single old example.
8. *(bonus, lower priority)* Create two hot loops back-to-back in time — if pad7's mystery bytes increment sequentially, that's strong evidence for the "ordering counter" theory.
9. **XDJ-RX2 cross-check.** Take a stick that already has steps 1-7's rekordbox-desktop-written loops on it, snapshot the ANLZ files, then set/edit a hot loop directly on the XDJ-RX2 itself (same track or a fresh one), re-export/re-import through rekordbox as usual, and snapshot again. Diff the hardware-written entry against the desktop-written one from step 1 for the same kind of loop. Confirms (or disproves) whether the byte layout this plan is reverse-engineering from desktop software also holds for hardware-authored loops — the actual real-world case for a DJ who sets a loop mid-set on the XDJ-RX2 rather than at home in rekordbox.

## After this

Once the currently-LOW/untested fields above have 2-3 real confirming
examples (matching the hot-cue writer's own bar), extend:

- `RawHotCueEntry` — add isLoop/loopEndMs/quantization fields
- `AnlzCueCodec::encodeHotCues`/`decodeHotCues` — loop-aware encode, with a real round-trip test against the newly captured bytes (same pattern as `tests/anlz_cue_codec_test.cpp`'s existing real-data cases)
- `RekordboxCueWriter::writeHotCues` — branch loop entries into the write
- `AddCueController` — drop the rekordbox-loop refusal, update its doc comment

## Explicitly out of scope here

OneLibrary's loop columns (`isActiveLoop`/`outUsec` in its `cue` table) —
that whole format's field mapping is already flagged low-confidence in
`docs/onelibrary-format.md`; needs its own separate verification pass, not
bundled into this one.
