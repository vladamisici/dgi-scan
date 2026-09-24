# Scantailor-DGI diagnostics log format

The application writes one JSON object per line (JSONL, UTF-8, `\n` line ends) to

    <log dir>/scantailor-perf-<yyyyMMdd-HHmmss>-<pid>.jsonl

When a file reaches 64 MB, writing continues in `...-<pid>.2.jsonl`, `.3.jsonl` and so on.

`<log dir>` is the crash-report directory:

- `%LOCALAPPDATA%\scantailor-dgi\scantailor-dgi\crashes` for the installed build;
- `<exe dir>\config\crashes` for the portable build;
- `<work dir>\instN\logs` for a stress run.

Log files older than 14 days are deleted when the application starts.

## Levels

| Level | Chosen by | What is written |
|---|---|---|
| `off` | `SCANTAILOR_DIAG=off` or `[diagnostics] level=off` in the settings INI | nothing |
| `basic` (default) | default | `stall`; `op` only when slow (≥ 50 ms on the GUI thread, ≥ 2000 ms elsewhere); `agg`, `beat` and `res` every 30 s |
| `verbose` | `SCANTAILOR_DIAG=verbose`, `[diagnostics] level=verbose`, and always in stress mode | every `op`; `agg`, `beat` and `res` every 1 s |

The environment variable overrides the INI.

## Fields on every record

| Field | Type | Meaning |
|---|---|---|
| `t` | number | milliseconds since diagnostics started, monotonic, excludes sleep/hibernation, 3 decimals |
| `ev` | string | record type, see below |
| `th` | string | thread role: `gui`, `worker`, `thumbs`, `bgexec`, `diag`, or `other` |
| `tid` | number | OS thread id |

## Record types

### `start` (first record)

`wall` (local ISO-8601 time with offset), `pid`, `app`, `version`, `level`, `exe`, `log_dir`,
`os` (pretty name), `os_kernel` (e.g. `10.0.22631`), `cpu` (brand string), `cores` (logical processors),
`ram_mb`, `qt` (runtime version), `portable` (bool).

### `screen` (one per screen, right after `start`)

`index`, `name`, `primary` (bool), `w`, `h` (logical pixels), `dpr` (device pixel ratio),
`logical_dpi`, `physical_dpi`, `refresh_hz`.

### `settings` (one, written by the application after `start`)

Each application setting that can affect performance, as `key: value`:
`auto_save_project`, `auto_save_interval_sec`, `crash_recovery`, `opengl`, `low_res_display`, `thumbnail_quality_w`,
`thumbnail_quality_h`, `max_logical_thumb_w`, `max_logical_thumb_h`, `batch_threads` (the number of
worker threads actually used: the setting, capped at the logical CPU count), `batch_threads_setting` (the
stored value), `worker_thread_priority` (`normal` or `low`), `highlight_deviation`, `single_column_thumbs`,
`color_scheme`, `tiff_bw_compression`, `tiff_color_compression`, `settings_file`.

### `op`: one timed operation

| Field | Meaning |
|---|---|
| `name` | operation name, `area.op[.phase]` (catalogue below) |
| `dur` | duration, ms |
| `depth` | nesting depth on that thread, 1 = outermost |
| `task` | background task id (0 on the GUI thread or outside a task) |
| ... | operation-specific attributes |

Scopes nest, so a parent's `dur` includes its children's. Within one `task`, the `stage.*` records
are nested: `stage.fix_orientation` contains `stage.page_split` contains ... `stage.output`.
A stage's own time is its `dur` minus the `dur` of the next-deeper `stage.*` record with the same `task`.
`stage.*` records are written at every level, basic included, so this always works.

A `task.run` whose `outcome` is anything other than `ok` or `cancelled` is also written at every level:
at basic level, `task.run` records are the slow tasks plus all failures, not every task.

### `agg`: aggregate of an operation since the previous `agg`

`name`, `n` (calls), `sum` (ms), `max` (ms). Several `agg` records can carry the same `name` (one per
call site). Sum them. Every `op` name also appears in `agg`, including ops that were not written
individually in basic mode, so `agg` is the complete accounting.

### `stall`: the GUI thread did not respond

| Field | Meaning |
|---|---|
| `dur` | ms between the last heartbeat before the stall and the first one after it, as recorded by the heartbeat itself (the watchdog's own delays do not add to it). The heartbeat runs every 50 ms (a coarse timer at basic level, which adds up to ~15 ms of jitter), so `dur` overstates the stall by up to ~65 ms. A phase change (`setPhase`) also ends a stall. If the whole process was frozen (debugger, suspended, laptop standby), that time is not counted. |
| `at` | `t` of the last heartbeat before the stall; the key that links `stall_progress` records to their `stall` |
| `phase` | `startup` (main window still being built), `run`, or `shutdown` (window closing). Only `run` stalls are ones an operator waits through; the analyser reports the others separately. |
| `ops` | array of operation names open on the GUI thread when the stall was detected, outermost first. Empty means no instrumented operation was open. |
| `ops_last` | the same, sampled just before the stall ended |
| `stack` | array of frames `module+0xRVA` or `module!function+0xOFFSET`, innermost first, captured once the stall passes 1 s. Absent for shorter stalls. |
| `dump` | path of a minidump, if one was written (verbose mode, stall ≥ 15 s) |
| `ongoing` | `true` if the stall was still in progress when diagnostics stopped |

Modal dialogs are **not** stalls: they run their own event loop and the heartbeat keeps going.

### `stall_progress`: a stall that has not ended yet

Written as soon as the stack of a stall is captured (or a stall dump written), then every 10 s while
the stall lasts. Same fields as `stall` (`dur` is the length so far). A `stall` record with the same
`at` follows when the stall ends. If the log ends with a `stall_progress` and no matching `stall` and no
`stop`, the process was ended during that stall (typically from Task Manager, or by a test timeout) -
it lasted at least that `dur`, and the record holds the stack of what the GUI thread was doing.

### `beat`: heartbeat statistics since the previous `beat`

`n` (heartbeats), `late50`, `late100`, `late250`, `late1000` (heartbeats that arrived more than that many ms late),
`max_late` (ms). A responsive GUI thread shows nearly everything below 50.

### `res`: process and system resources

| Field | Meaning |
|---|---|
| `reason` | `periodic`, `final`, `cycle_end`, ... |
| `private_mb` | private committed bytes (commit charge), MiB. **Primary leak metric.** |
| `ws_mb`, `peak_ws_mb` | working set, peak working set, MiB |
| `handles` | kernel handles |
| `gdi`, `user` | GDI and USER objects |
| `threads` | threads in the process |
| `cpu_pct` | process CPU since the previous sample, % of the **whole machine** (all cores = 100) |
| `cpu_s` | cumulative process CPU seconds |
| `io_read_mb`, `io_write_mb`, `io_other_mb` | cumulative process I/O, MiB |
| `io_read_ops`, `io_write_ops` | cumulative I/O operation counts |
| `sys_avail_mb` | available physical memory on the machine |
| `sys_load_pct` | machine memory load, % |
| `sys_commit_mb`, `sys_commit_limit_mb` | machine commit charge and limit |
| `log_queue`, `log_dropped` | diagnostics backlog, and records dropped because the backlog was full |

Extra attributes given to `sampleNow()` (for example `cycle`) are added.

### `phase`

`phase`: the application entered `run` (event loop started) or `shutdown` (main window closing).

### `oom`

The application ran out of memory. A `res` record with `reason: "oom"` follows.

### `stop` (last record)

`stalls` (count), `longest_stall` (ms), `records`, `dropped`.

### Stress-driver records (only in `--stress` runs)

| `ev` | Fields |
|---|---|
| `stress.config` | `instance`, `cycles`, `mode`, `pages`, `scans`, `work`, `nav_pages` |
| `stress.generate` | `count`, `kind`, `dpi`, `compression` (libtiff code: 5 LZW, 4 CCITT G4 - fixed, not the operator's setting), `dur`, `ok` |
| `stress.cycle_begin` | `cycle` |
| `stress.step` | `cycle`, `step` (name), `dur`, `ok`, plus step attributes |
| `stress.batch` | `cycle`, `stage` (0-5), `stage_name`, `pages`, `dur`, `ok`, `sec_per_page`, `rebatch` (`true` for the Output batch of a reopened title, where every page is already done: it measures confirming the cache, not processing) |
| `stress.page_load` | `cycle`, `stage`, `stage_name`, `index`, `dur` (request until the page is on screen), `outcome` (`ok`, `load_error`, `task_failed`, `output_not_ready`, `timeout`) |
| `stress.stage_switch` | `cycle`, `from`, `to`, `dur` (until the selected page is on screen) |
| `stress.thumb_scroll` | `cycle`, `stage`, `steps`, `dur` |
| `stress.autosave` | `cycle`, `dur`, `ok` (whether anything was written), `branch` (`project_file`, `snapshot`, `unsaved_session`, `batch_skip`, `guard_skip`, `disabled`, `busy`) |
| `stress.cycle_end` | `cycle`, `dur`. A `res` record with `reason: "cycle_end"` and `cycle` is written right before it, after the project has been closed and the process has settled. |
| `stress.unexpected_dialog` | `class`, `title`, `text`, `action` |
| `stress.failure` | `what`, `cycle`, `step` |
| `stress.finished` | `ok` (bool), `cycles`, `failures`, `dur`, `exit_code` |

## Operation catalogue

GUI thread (`th: gui`):

| Name | Where |
|---|---|
| `project.autosave` (attrs `branch`: `project_file`, `snapshot`, `unsaved_session`, `batch_skip`, `guard_skip`, `disabled`, `busy`; `ok`: whether anything was written) | periodic autosave |
| `project.write_quiet` (`target`, `ok`) | autosave/snapshot write |
| `project.recovery_snapshot` | writing the `.autosave` sidecar |
| `project.compare_files` (`bytes`) | byte comparison of two project files |
| `project.save` (`ok`) | Save / Save As |
| `project.close.backup_write`, `project.close.compare`, `project.close.rename` | closing a project (no scope spans the whole close: it can wait on the save prompt) |
| `project.open.parse_xml`, `project.open.reader`, `project.opened` | opening a project |
| `project.switch` (`pages`) and phases `.retire`, `.stages`, `.thumb_cache`, `.reset_thumbs`, `.update_main_area` | switching project |
| `project.release_retired` | freeing the previous project's objects |
| `project.writer.write` (`bytes`, `ok`) with phases `.build_dom`, `.serialize`, `.commit` | any thread |
| `ui.stage_switch` (`from`, `to`) | switching processing stage |
| `propagate.content_box`, `propagate.page_orientation` | stage-switch propagation |
| `thumbs.reset`, `thumbs.sequence.reset` (`pages`), `thumbs.invalidate_all` | thumbnail list rebuild |
| `ui.load_page` | starting an interactive page load |
| `ui.filter_result`, `ui.update_ui` (`stage`) | a finished task's result reaching the GUI |
| `batch.start` (`pages`), `batch.stop` | batch processing |
| `ui.new_open_panel` | start page (recent-projects list) |
| `history.read`, `history.write`, `history.is_available` (`path` when ≥ 50 ms) | project history |
| `thumbs.cache.destroy` | joining a thumbnail-cache thread |
| `pool.shutdown` | waiting for worker threads at exit |

Aggregated only (`agg`), because they are too frequent to write one by one:
`thumbs.factory.get`, `cache_task.output`, `thumbs.load_result`, `ui.paint.image_view`,
`ui.paint.thumbnail`, `ui.image_view.ctor`, `task.deliver` (worker → GUI event latency), `bgexec.task`,
`ui.downscale` (making a view's reduced copy, on a worker thread).

`ui.hq_transform` (attr `src_mpx`: megapixels of the image it renders from - the full image, or the reduced
copy with low-resolution display on) is an `op` on the `bgexec` thread: rendering the sharp version of a view.

Worker and helper threads:

| Name | Thread | Where |
|---|---|---|
| `task.run` (`type`: `interactive`/`batch`, `queue_ms`, `outcome`: `ok`, `null`, `bad_alloc`, `exception`, `unknown`, `cancelled`) | worker | one background task |
| `task.load_file` | worker | load + thumbnail + stage chain |
| `image.load` (`w`, `h`, `mb`) | worker, thumbs | decoding an image file |
| `stage.fix_orientation` ... `stage.output` | worker | processing stages (inclusive, see above) |
| `output.generate`, `output.write` | worker | output generation and writing |
| `tiff.write` (`w`, `h`, `mb`, `ok`) | worker | writing a TIFF |
| `thumbs.ensure_exists`, `thumbs.recreate` | worker | thumbnail files |
| `thumbs.bg.load` (`source`: `cache` / `image`) | thumbs | background thumbnail loading |
| `file.atomic_commit` (`durable`, `ok`) with phases `.flush`, `.sync`, `.rename` | any | atomic file replacement |
| `file.rename` (`attempts`, `slept_ms`, `error`, `ok`) | any | rename with retries |

Notes:

- `task.run` `outcome: "cancelled"` means cancelled before it started. A task cancelled while running reports `null`,
  because the loading task turns the cancellation into an empty result.
- `file.rename` `error` is the Windows error of the last failed attempt, even when a later retry succeeded (`ok: true`);
  0 when the first attempt worked.
- Closing the temporary file inside `file.atomic_commit` falls between its phases, so it shows only in the parent's own time.
- `stress.*` operation names (for example `stress.create_metadata`) are work done by the test driver itself. A stall
  whose `ops` start with `stress.` is attributed to the test, not to the application.
