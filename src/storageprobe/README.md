# storageprobe

Measures a removable drive (USB stick, SD card, portable SSD) the way an
application actually uses it, and turns the numbers into a workload
estimate. Standalone on purpose: plain C++17, no Qt, no dependency on
the rest of this repository. Copy the folder into another project and
add the `.cpp` files to a target; nothing else is needed.

Three read measurements, on files already on the drive:

- **streaming**: a long sequential read from the front of each large
  file (an audio track, a RAW photo);
- **random reads**: 4 KiB at random aligned offsets, through direct I/O
  so the page cache never answers (a database page, a thumbnail inside a
  RAW, an index block);
- **small file open + read**: open, read 16 KiB, close, on each small
  file (an analysis file, a sidecar, a JPEG preview).

Three write measurements, in a hidden scratch folder the probe creates
and removes:

- **streaming write**, large files written in 1 MiB pieces and flushed;
- **small file write**, 16 KiB files each written, flushed and closed;
- **in-place update**, 4 KiB overwrites at random offsets of one file,
  each flushed.

`surface_check.hpp` reads every file on the drive once, the way a backup
would, and reports files that could not be read and files that read at
under a tenth of the drive's own median rate. Flash does not expose
wear counters over USB mass storage; what it does show, months before a
block fails, is the controller retrying error correction on weak cells,
which makes those reads abnormally slow. The read probe counts the same
symptom in its own tail: reads over five times the median.

`workload.hpp` models a session as a list of actions (so many random
reads, so many small opens, so many bytes streamed, so many times), sums
the modelled waiting for the measured drive and for a reference drive,
and reports a score (100 = the reference drive), a speed class by
today's standards, and a perceptual verdict per action from thresholds
the caller sets. What the actions are is the caller's business: a DJ set
here, an import session in a photo tool.

Cancellation is a `std::function<bool()>`; the probes poll it between
files and throw `storageprobe::Cancelled`. Files that fail to open are
skipped, never fatal. A measurement field of zero means "not measured".
Direct I/O is Linux (`O_DIRECT`) and Windows (`FILE_FLAG_NO_BUFFERING`);
elsewhere the random reads fall back to buffered reads and measure less
than they claim. Page-cache dropping before a buffered read is Linux
only (`posix_fadvise`).
