# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`gpuwho` is a single-binary C11 CLI for shared GPU servers. It links directly
against `libnvidia-ml` (NVML) and reads `/proc` to answer: who is using the
GPUs right now, when one frees up, and who used how much over a period. See
[README.md](README.md) for full user-facing behavior, output formats, and the
attribution caveats (Docker, CUDA MPS, short-lived processes) — read it before
changing report/collect semantics.

## Build, test, install

```sh
make                        # build ./gpuwho
make config                 # show which nvml.h / link target the build found
make CUDA_HOME=/opt/cuda    # point at a non-standard CUDA install
make test                   # build + run tests/run.sh (64 cases, no GPU needed)
make install                # /usr/local/bin/gpuwho + man page
make install-conf           # /etc/gpuwho/ignore.conf (never overwrites)
make install-units          # systemd units (accounting service, collect timer)
make deb                    # build the .deb via dpkg-deb + fakeroot
make clean
```

Requires a C11 compiler and `nvml.h` (from `cuda-nvml-dev` or
`nvidia-cuda-dev`). The binary always links `libnvidia-ml` dynamically since it
must match the running driver — `make config` shows which link target
(`-lnvidia-ml`, the CUDA stub, or a versioned path) was chosen.

The binary has a `DT_NEEDED` on `libnvidia-ml.so.1`, so it will not even start
(not even `--version`) without that library loadable — on a machine with no
NVIDIA driver, symlink the CUDA stub as `libnvidia-ml.so.1` to satisfy the
loader (see `.github/workflows/release.yml` for the exact CI recipe).

There is no test framework beyond `tests/run.sh` (POSIX sh): it builds
synthetic JSONL event logs under a temp state dir and drives `gpuwho report`
against them, so the accounting-dependent code paths are covered without root
or a GPU. To run it directly against an already-built binary:

```sh
sh tests/run.sh ./gpuwho
```

There is no way to run a single test case in isolation — `run.sh` is one
linear script of `check`/`field` assertions; to target one area, comment out
the unrelated sections while iterating, or grep the script for the relevant
`report` scenario.

Releasing is tag-driven: bump `VERSION` in [Makefile](Makefile), `make deb` to
sanity-check locally, tag `vX.Y.Z` matching `VERSION` exactly, push the tag —
CI (`.github/workflows/release.yml`) rebuilds and publishes the `.deb` to
GitHub Releases; a mismatched tag/VERSION fails the build on purpose.

## Architecture

Everything is C11, one flat `src/` directory, no external deps beyond libc and
NVML. Shared declarations for every module live in
[src/gpuwho.h](src/gpuwho.h) — read it first; each section is a layer:

- **`nvml_layer.c`** — the only file that includes `<nvml.h>`. Wraps device
  enumeration, the compute-process snapshot, and the accounting-buffer lookup
  (`gw_acct_lookup`). Never hand-declares NVML prototypes, so whatever symbol
  version the installed header maps to (`_v2`/`_v3`/...) is what gets linked,
  keeping struct layouts in sync with the header used at compile time.
- **`procinfo.c`** — resolves a pid (which NVML gives with nothing else
  attached) into uid/user/cmdline/start-time via `/proc`. Every read here
  races the process exiting; `ENOENT` is expected, not an error.
- **`filter.c`** — the ignore-rule engine (`name`, `cmd:glob`, `user:x`,
  `uid:n`, `--min-mem`). Only applies to processes that hold a genuine compute
  context; graphics-only processes are already excluded by NVML itself and
  never reach this layer. A rule only affects whether tracking *starts* — an
  already-tracked process is never dropped mid-run by a later rule change or
  by memory drifting across `--min-mem`.
- **`json.c`/`json.h`** — a minimal hand-rolled JSON reader (value tree) and
  writer (growable buffer + container stack). Used for both `state.json` and
  event-log lines; no external JSON library.
- **`state.c`** — `state.json`, the collector's only memory between ticks
  (it's a one-shot process). Rewritten atomically (temp file + `rename`).
- **`logs.c`** — enumerates/opens the event log files
  (`events-YYYY-MM.jsonl[.gz]`, one per month, transparent gzip). Shared by
  `report`, `prune`, and the log-size warning so they all see the same view.
- **`cmd_*.c`** — one file per subcommand (`snapshot`, `wait`, `collect`,
  `report`, `setup`, `prune`), each exposing a single `gw_cmd_*` entry point
  called from [src/main.c](src/main.c). `main.c` peels off global options
  (`--state-dir`, `--ignore*`, `--min-mem`, `--log-max-size`, `--no-color`,
  `-h`/`-V`) from anywhere on the command line before dispatching to the
  subcommand, or falls through to `snapshot` if no subcommand is given.

### The event log is the only durable state

There is no database. A plain file can't update a row in place, so a process's
lifecycle is two independent append-only JSONL events, `start` and `end`,
joined **at read time** on `(gpu, pid, pst)` — `pst` (process start time) is
required because pids recycle within days, and `gpu` is part of the key
because one process on two GPUs produces two intervals.

```
        every 60 s (systemd timer)
 NVML ──┐
        ├─> gpuwho collect ──> state.json   (open intervals + last_tick)
 /proc ─┘         │
                  └──────────> events-YYYY-MM.jsonl  (start/end events)
                                        │
 gpuwho report <────────────────────────┘
```

- `collect` (`cmd_collect.c`) is invoked once a minute by the systemd timer;
  it diffs the current NVML snapshot against `state.json`'s open intervals,
  emits `start` events for new ones and `end` events for closed ones, and
  looks up NVML's accounting buffer (`gw_acct_lookup`) for the end metrics.
- An end event's `src` field says where its data came from: `"acct"` means the
  accounting buffer had it (`end = startTime + time`, metrics present);
  `"tick"` means it didn't (buffer overflow, reboot, accounting never
  enabled), so `end = last_tick` with no metrics. Reboots self-resolve this
  way: on the first tick after boot every previously-open process closes with
  `src:"tick"`.
- `report` (`cmd_report.c`) only reads the log — nothing needs to be running
  to generate a report — and performs the `(gpu, pid, pst)` join and
  aggregation itself.
- Every log line carries a schema version (`v`) and unknown fields are
  ignored on read, so the format can gain fields (e.g. a future Slurm `job`
  id) without breaking older logs or older binaries reading newer ones. See
  the README's "Extending: Slurm" section for the exact seam
  (`cmd_collect.c`, where the `start` event is assembled) if asked to add
  job-id attribution.
- Retention is plain file deletion (`prune`, `cmd_prune.c`) — no compaction,
  no vacuum. Pruning takes the collector's lock so a month can't be deleted
  mid-append.

### Conventions worth preserving

- Functions and types are prefixed `gw_` (or `jb_`/`json_` in the JSON
  module); there is no namespacing beyond the prefix.
- Fixed-size buffers (`GW_USER_MAX`, `GW_CMD_MAX`, etc.) are used throughout
  instead of dynamic string growth for hot-path structs like `gw_procinfo`.
  `gw_snapshot`/`gw_state.open` do grow dynamically (`gw_realloc`).
  `GW_MEM_UNKNOWN`/`GW_UTIL_UNKNOWN` sentinel values (not a separate
  `bool`/`optional`) mark metrics that couldn't be read.
- `gw_die`/`gw_warn` are the only error-reporting paths (`gw_die` is
  `noreturn`); there's no exception-like control flow.
- New `CFLAGS`/`LDFLAGS` should be added via `EXTRA_CFLAGS`/`EXTRA_LDFLAGS` on
  the `make` command line, not by overriding `CFLAGS` directly — the Makefile
  computes the NVML include path into `CFLAGS`, and a plain override drops it.
