# A page waiting for a read: one rule

Most pages start work on a background thread: they read a stick's
catalogs, list the backup folder, preview a backup. While that work is
running the page says so, with a busy overlay, a progress bar or
"Reading this stick...". The failure this document is about is a page
that stays in that state when nothing is running any more. The report
that started it: Metadata Backup sat on "scanning" for good on the first
open after a stick went in, with no thread doing any work. Cancel did
nothing until the read it was queued behind had finished.

Every controller did this in its own way. Some used a bool, some
`QFutureWatcher::isRunning()`, some a queue. A second request was
silently dropped, even when the method told its caller yes. Cancel was
only over once the worker had noticed it. A destructor either waited for
the worker, which froze the window when the worker could not see the
cancel, or did not cancel it at all. None of them heard about a pulled
stick. Each variant was correct as long as its worker behaved, and wrong
as soon as the worker did not.

## The rule

For a read a page is waiting for:

1. **busy is derived.** It is true exactly while the controller holds an
   outstanding request. There is no separate flag to forget to clear,
   and `isRunning()` is never used. `isRunning()` goes false before the
   result has been delivered and applied.
2. **Every request ends exactly once**, on the controller's thread, in
   one of three ways: a result, an error, or cancelled. A worker that
   throws ends in an error. Only the end of a request clears busy.
3. **The same request again is served by the one already running.** It
   is accepted, it starts no second read, and nothing is dropped.
4. **A different request supersedes the running one.** The old token is
   cancelled and whatever the old worker returns is ignored: it is never
   applied and it never touches busy. The page's new scope, such as the
   stick or the format, is stored when the request is accepted, not
   before a check that might refuse it.
5. **Cancel ends the request at once.** The worker is told through its
   token, and its late answer is ignored. A worker stuck where it cannot
   look at its token, for example a hung device or a slow folder, can no
   longer hold the page.
6. **A stick that goes away ends every request reading it**, in an
   error. MediaController announces the removal through `StickEvents`,
   and every request listens there.
7. **Destruction cancels everything and waits a bounded moment.** A
   worker that honours its token stops within a row, so ordinarily no
   task is left reading a stick for a page that is gone (the Library
   Health pages have held to that since they learned to stop within a
   row). A worker that has not stopped after a second, because it cannot
   look at its token, is let go instead of freezing the window. It never
   touches its controller: it holds its token, its arguments, and the
   process-wide catalog cache, so it can finish on its own, and its answer
   goes nowhere. `main()` waits for the thread pool before the process
   exits.

Two refinements. First, a request whose scope is the one outstanding is
recognised before anything is reset: a request that turns out to be
served must not reset the progress bar or take the reporter away from
the read that answers it. Second, a read asked for while the page's own
write runs is not started beside the write. The page disables the
controls that would ask for one. A rescan the page asks for itself is
put off until the write is over (Clean Up's `m_rescanAfterWrite`).

`gui::AsyncRequest<Result>` (src/gui/async_request.hpp) implements the
rule, so a controller does not have to rebuild it. Each request gets its
own `QFutureWatcher`. Reusing one watcher with `setFuture()` loses a
finished signal that has already been sent, and a lost "done" is exactly
what this rule exists to prevent. The workers run on a pool of their own
(`asyncRequestPool()`), not the global one the writes use. A worker that
was let go keeps its thread until its I/O returns. On a shared pool, a
hung stick and a few cancels would take every thread, and a save would
queue behind them and never start.

```cpp
AsyncRequest<ScanResult> m_scan{this, [this]() { emit busyChanged(); }};

m_scan.start(libraryPath, stickRoot,
             [libraryPath](CancellationToken cancel) { return runScan(libraryPath, cancel); },
             {[this](ScanResult &&r) { apply(std::move(r)); },
              [this](const QString &error) { showError(error); },
              [this]() { showCancelled(); }});
```

## Writes are different

Rule 5 does not apply to a write. A write that is half done cannot be
declared finished. Its cancel is a request the writer acknowledges, and
the page stays busy until the writer returns (see write-path-rules.md and
the save pattern). What a write does share with reads:

- Rule 2: it ends exactly once.
- A lock it takes is released by an RAII hold (`DirectWriteHold`), never
  by a finished handler that might not run.

A write whose page goes away while it runs is neither abandoned with its
lock still held nor waited for on the GUI thread. The page is often gone
because the stick-gone dialog popped it, which is exactly when a write
can be stuck on a device that is not there. `finishWriteDetached()`
(src/gui/detached_write.hpp) hands the write and its locks to a watcher
owned by the application. That watcher gives the locks back when the
write returns. A write is cancelled on the way out only where its own
Cancel is safe at any moment. Clean Up's deletes stop between files. The
Engine library creation is not cancelled there, because whether it has
reached its copy to the stick is something the GUI thread only learns
from a signal still in flight.

## Testing it

The QML harness's `catalogGate` fixture holds a stick's rekordbox read
at a gate, so a test can act while the read is still running:

- `hold(tracks, honourCancel)`: `honourCancel` false stands for a read
  that cannot notice its token.
- `release()` and `releaseAfter(ms)`.
- `announceStickGone(root)`.

Every controller brought under the rule gets these cases:

- a read that ends clears busy;
- cancel during a read that cannot notice ends busy at once, and asking
  again is answered by a new read;
- a different request during a read supersedes it;
- the same request again is served by the running read;
- two pages on one stick share one read;
- a pulled stick ends its read, and another stick being pulled does not;
- leaving the page mid-read stops the read, and does not wait for one
  that cannot stop.

Every case ends by checking that once nothing is reading, busy is off.
tests/qml/tst_MetadataBackupRequests.qml is the first of them.
