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
   goes nowhere. See "The end of the process" below for what happens to
   one still running when the app quits.

Some refinements:

- A request whose scope is the one outstanding is recognised before
  anything is reset. A request that turns out to be served must not
  change the page's scope, reset the progress bar or take the reporter
  away from the read that answers it.
- A progress reporter speaks only while its own request is the
  outstanding one: `AsyncRequest::speaksForNext()`, taken right before
  the request starts. A superseded or cancelled read reports until its
  worker notices, and must not move the bar.
- A read asked for while the page's own write runs is not started beside
  the write, and it is not dropped either. It runs once the write is
  over: Clean Up's `m_rescanAfterWrite`, Metadata Backup's
  `m_scanAfterSave`.
- A request that reads more than one stick names all of them
  (`startOnSticks()`). Clone's preview reads its source and its target,
  and pulling either one ends it.
- A worker that can look at a token does. The Stick Backup, Clone and
  Restore previews take none of their own, so they stop at the start and
  between phases, and hand the token to the walk inside.

`gui::AsyncRequest<Result>` (src/gui/async_request.hpp) implements the
rule, so a controller does not have to rebuild it. Each request gets its
own `QFutureWatcher`. Reusing one watcher with `setFuture()` loses a
finished signal that has already been sent, and a lost "done" is exactly
what this rule exists to prevent. Each worker runs on a thread of its
own (`AsyncWorkers`), not on a pool. A worker that was let go keeps its
thread until its I/O returns, so in any pool with a cap, enough of them on
a hung stick would take every thread. Every later read, and on the global
pool every save, would then queue behind them and never start. A page
asks for a read when it opens or when someone picks something, so there
are never many threads.

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
owned by the application. When the write returns, that watcher does what
the page's own handler would have done without the page, for example
telling the catalog cache that a restored or cloned stick was rewritten.
Then it gives the locks back.

A write is cancelled on the way out only where its own Cancel is safe at
any moment:
- Clean Up's deletes stop between files.
- A stick backup, a clone or a restore rolls back or leaves its journal
  for recovery.
- The Engine library creation is not cancelled, because whether it has
  reached its copy to the stick is something the GUI thread only learns
  from a signal still in flight.

Work a page accepted but had not started yet goes on too: deletes queued
on Manage Backups run in the background when the page closes.

## The end of the process

A worker let go of on a pulled stick can still be running when the app
quits, possibly inside `LibraryCatalogCache` or the SQLite and Kaitai code
under it. If `main()` simply returned, the static destructors would
destroy those under it: a use after destroy. So `main()` ends in this
order:

1. `AsyncWorkers::beginShutdown()`. From here on a request going away
   waits for nothing, since there is one wait for all of them below.
2. The QML engine is destroyed, and with it every page and controller.
   Each cancels the reads it was waiting for. A controller with a write
   still running hands the write on (`finishWriteDetached()`) and does
   not drop it.
3. The global pool, where the writes run, is waited for up to 15
   seconds, as before.
4. `exitAfterAsyncWork()` waits up to 2 seconds for the workers that were
   let go. If one is still running, the process ends with
   `std::quick_exit()`, which runs no static destructors. Nothing is
   lost: a read writes nothing, and the writes were waited for in
   step 3.

`async_request_exit_test` runs the same sequence in a child process with
a worker stuck for good and a static that aborts if it is destroyed
while a worker runs. The child has to exit cleanly.

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
