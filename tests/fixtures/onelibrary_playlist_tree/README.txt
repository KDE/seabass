rekordbox 7.2.18's exportLibrary.db before it created any playlist
(0-base, with its -wal as rekordbox left it), and what its database held
after each later step (expected.tsv), for issue #62. The steps and the
stick are those of tests/fixtures/pdb_playlist_tree: create Q1, a long
Unicode name first in the level, folder F1 holding F1A, a05 to the top of
Q1, delete Q1, delete F1. expected.tsv was read from rekordbox's own
snapshot after each step: path, sequenceNo, folder, content ids in order.
Tests copy 0-base before opening it.
