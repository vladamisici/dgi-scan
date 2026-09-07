# Crash safety, autosave and memory

This fork adds crash diagnostics, real autosave and crash recovery, and cuts the
memory ScanTailor uses on large titles. No image-processing behaviour is
changed: the same input produces the same output files.

It exists because of a specific production failure. In a shop running ~30,000
titles, operators correcting titles of 50+ pages reported that ScanTailor
"closes before you can save", losing the session's corrections. The count went
from 176 to 304 affected titles in a few days, and correlated with home-office
work — remote desktop, VPN, and projects living on network shares.

## Why it was happening

Five independent defects, each sufficient on its own to lose a session:

**1. Any exception other than `std::bad_alloc` killed the process silently.**
Every thread boundary caught only `std::bad_alloc`. Anything else — a
`std::runtime_error` from a truncated TIFF on a flaky share, a `std::logic_error`
from the zone editor, an `std::invalid_argument` from a degenerate geometry —
escaped the thread and reached `std::terminate`, which ends the process with no
dialog, no log, and no chance to save. "It closes before you can save" is the
literal symptom.

**2. There were no crash diagnostics at all.** No minidump, no terminate handler,
no log file — and because the binary is built as a GUI application, the 19
`qWarning()` calls in the codebase went nowhere. A support team had no way to
tell an out-of-memory kill from a driver fault from a dropped RDP session, which
is why the problem kept recurring instead of being fixed.

**3. Saving truncated the project before writing it.** `ProjectWriter::write()`
opened the destination with `QIODevice::WriteOnly`, which empties the file the
instant the write begins, and then returned `true` without checking that a single
byte reached the disk. On a share over VPN, an interruption anywhere in that
window left a zero-length or half-written project — and the autosave did this to
the live project file on a timer.

**4. Autosave was off by default, and when on, barely ran.** The timer was armed
only from `currentPageChanged()`, so an operator correcting one page — exactly the
workflow that loses work — was never autosaved after the first minute. "Apply to
all pages", the single largest edit, never armed it at all.

**5. Batch processing built large amounts of memory it never used.** For every
page, a `DespeckleState` converted the output image to RGB32 — a 32× inflation of
1-bit output, roughly 140 MB for a 600 dpi A4 page — plus two downscaled copies
of the page. All of it exists only to draw the despeckling UI, and
`UiUpdater::updateUI()` returns immediately during batch processing. On a
four-thread machine that is most of a gigabyte of pure waste.

## What changed

### Work is no longer lost

- **Project files are written atomically.** `ProjectWriter` now writes to a
  temporary file beside the target, flushes it to disk (`FlushFileBuffers` /
  `fsync`), and only then renames it into place. An interrupted save now costs
  the save, not the project. Write errors are detected and reported instead of
  being silently swallowed. The same applies to default-parameter profiles.
- **Autosave is on by default**, every 2 minutes (configurable, 15 s–1 h), on a
  repeating timer that runs for as long as a project is open.
- **A crash-recovery snapshot** is kept beside the project as
  `<project>.ScanTailor.autosave`. It is written on the same timer and when
  Windows signals that the desktop session is ending (a logoff), and it is
  removed whenever the project is saved or closed deliberately. If one is found
  when the project is next opened, ScanTailor offers to restore it. A snapshot is
  an ordinary project file — renaming it over the project also works.
- **Projects that have never been saved are protected too**, via a snapshot in
  the user's local application data. It is offered back at the next start.
- **Answering "Save" at the close prompt now aborts the close if the save did not
  happen** — previously, cancelling the file dialog or a failed write discarded
  the project anyway.
- Autosave no longer fires inside modal prompts (which could make "Don't Save"
  silently not discard), never shows a dialog of its own, and stands down during
  batch processing.

### Crashes leave evidence

`CrashHandler` is installed before anything else happens and writes to
`<config>/crashes/` (portable install) or `%LOCALAPPDATA%\scantailor-advanced\crashes\`:

- `scantailor.log` — timestamped log of every `qDebug`/`qWarning`/`qCritical`,
  which previously went nowhere, plus start-up and project-open records.
- `scantailor-<date>-<pid>.dmp` — a minidump written by an unhandled-exception
  filter, a `std::terminate` handler (the one that matters — that is what fires
  when a worker thread lets an exception escape), and handlers for `abort()`,
  pure virtual calls and invalid CRT parameters.
- `scantailor-<date>-<pid>.txt` — a plain-text summary: reason, exception code,
  faulting address, bitness, and which project was open.

Release builds now ship a matching `.pdb`, without which a dump is only a list of
addresses. Optimisation settings are unchanged (`/Zi` plus `/DEBUG /OPT:REF
/OPT:ICF`).

### Crashes are rarer

- Every thread boundary now catches `std::exception` and `...` in addition to
  `std::bad_alloc`, logs it, and keeps the application alive. Losing one page's
  processing beats losing the operator's session.
- **A blank page with split output crashed the process outright.**
  `buildEmptyImage()` set the foreground mask but not the foreground type, so
  `OutputImageBuilder::build()` returned a plain image, the `dynamic_cast` to
  `OutputImageWithForeground` returned null, and it was dereferenced. Blank
  pages, inside covers and separator sheets are routine in book scanning. Fixed
  at the source, with a null guard at the cast as well.
- **`DeviationProvider` had no synchronisation**, while worker threads inserted
  into its `std::unordered_map` and the GUI thread read it to draw and sort
  thumbnails. A rehash during a concurrent lookup is a crash, and it gets more
  likely the more pages a project has. It is now internally locked; no computed
  value changes.
- The out-of-memory rescue dialog is now shown *before* the project is torn down.
  Previously the window was scheduled for deletion and the teardown — which
  allocates — ran first, so a second allocation failure made the application
  vanish instead of offering to save. The emergency reserve is 32 MB, sized for
  an actual project serialisation rather than the previous 3 MB.
- The thumbnail thread no longer spins retrying a failed allocation while the GUI
  thread is trying to allocate the rescue dialog.
- Shutdown waits for workers with a timeout instead of indefinitely, and project
  switching drains the worker pool so GUI-owned objects are never destroyed on a
  worker thread.

### Memory

- Display-only data — the RGB32 `DespeckleState`, the full-resolution images and
  their downscaled copies — is no longer built during batch processing.
- `ThumbnailSequence::reset()` built a graphics item for every page and then
  immediately built a second one and deleted the first. Loading a project now
  does that work once, halving both the allocations and the per-page file
  round-trips to the share.
- Qt returns a null image rather than throwing when an image allocation fails, so
  real out-of-memory conditions went undetected and turned into blank output or a
  crash further downstream. The largest allocations in `OutputGenerator` now
  raise a proper out-of-memory condition instead.
- 32-bit builds are linked `/LARGEADDRESSAWARE`, lifting their ceiling from 2 GB
  to 4 GB on 64-bit Windows.

## Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `auto_save_project` | on | Autosave writes the project file itself |
| `auto_save_interval_sec` | 120 | Applies to both autosave and snapshots |
| `crash_recovery` | on | Keep a recovery snapshot beside the project |

## Recovering the affected titles

For titles already damaged, in order:

1. Look for `<project>.ScanTailor.autosave` beside the project — on this build it
   is written every two minutes, so it holds the session up to shortly before the
   crash. Opening the project offers it automatically.
2. Look for `Backup.<project>.ScanTailor`, which older builds wrote when closing.
3. Check `crashes/scantailor.log` and the `.txt` next to any `.dmp` to find out
   what actually happened. If dumps show out-of-memory on 32-bit, move those
   machines to the 64-bit build.

Send a pilot batch back through the new build before returning all of them, and
keep an eye on the crash folder — the whole point of the logging is that the next
failure is diagnosable rather than invisible.

## Building

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows -DBoost_USE_STATIC_LIBS=OFF
cmake --build build --config Release --parallel
```

Boost's `unit_test_framework` is now required only when `BUILD_TESTING` is on, so
the application no longer fails to configure over a test-only dependency.
