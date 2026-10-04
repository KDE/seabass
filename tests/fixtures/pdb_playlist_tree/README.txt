rekordbox 7.2.18's own export.pdb at each step of creating, reordering and
deleting playlists on a stick (issue #62), MacBook, 2026-10-04. The stick
(TESTROWS) held the 34 generated tone tracks of the pdb reference kit and
no playlists. Only export.pdb is kept.

0-base.pdb          34 tracks, no playlists.
1-create-q1.pdb     Q1 created: a01..a05 (track ids 1..5).
2-long-name.pdb     a playlist with a 137-character Unicode name (a06..a08);
                    rekordbox rewrote the top level, the new one first.
3-folder.pdb        folder F1 holding F1A (b01, c01), again first.
4-reorder.pdb       a05 moved to the top of Q1; Q1's entries rewritten.
5-delete.pdb        Q1 deleted: its entries, and the top level rewritten.
6-delete-folder.pdb F1 deleted with F1A and F1A's entries.
