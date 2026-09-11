<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Kaitai Struct specs for rekordbox formats

`rekordbox_pdb.ksy` and `rekordbox_anlz.ksy` are vendored, unmodified, from
[Deep-Symmetry/crate-digger](https://github.com/Deep-Symmetry/crate-digger)
(`src/main/kaitai/`), fetched 2026-08-26. Licensed EPL-1.0 (see header
comments in each file); reverse-engineering credited there to
@henrybetts, @flesniak, @GreyCat and James Elliott (@brunchboy).

They describe the `PIONEER/rekordbox/export.pdb` (DeviceSQL) and
`PIONEER/USBANLZ/**/ANLZ*.{DAT,EXT,2EX}` file formats used on rekordbox USB
exports.

## Regenerating the C++ parser

The generated parser in `src/infrastructure/rekordbox/generated/` is
committed directly (kaitai-struct-compiler requires a JVM and isn't assumed
to be part of the normal build). To regenerate after editing a `.ksy` file,
run `tools/regenerate_kaitai.sh`.
