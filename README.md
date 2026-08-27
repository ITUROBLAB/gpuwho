# gpuwho

[![build](https://github.com/ITUROBLAB/gpuwho/actions/workflows/release.yml/badge.svg)](https://github.com/ITUROBLAB/gpuwho/actions/workflows/release.yml)
![version](https://img.shields.io/badge/version-0.1.2-blue)
![platform](https://img.shields.io/badge/platform-linux%20amd64-lightgrey)
![ubuntu](https://img.shields.io/badge/Ubuntu-20.04%20%7C%2022.04%20%7C%2024.04-E95420?logo=ubuntu&logoColor=white)
![standard](https://img.shields.io/badge/std-C11-blue)
![deps](https://img.shields.io/badge/runtime%20deps-libc%20%2B%20NVIDIA%20driver-brightgreen)

**Who is using the GPUs, when one frees up, and who used how much.**

A single-binary C tool for shared GPU servers. It answers three questions:

- **Who is using the GPUs right now?** — `gpuwho`
- **When will a GPU become free?** — `gpuwho wait`
- **Who used how much over a period?** — `gpuwho report`

It links directly against `libnvidia-ml` (the library behind `nvidia-smi`) and
reads `/proc` for the parts NVML does not know about, such as who owns a pid.

|  |  |
|---|---|
| **Version** | 0.1.1 |
| **Runtime** | Linux x86-64, an NVIDIA driver providing `libnvidia-ml.so.1` |
| **Build** | C11 compiler, GNU make, `nvml.h` (from `cuda-nvml-dev` or `nvidia-cuda-dev`) |
| **Packaging** | `dpkg-deb` + `fakeroot` — no debhelper |
| **Optional** | `nvidia-persistenced` (keeps accounting mode alive), `curl` (for `wait` notifications), `gzip` (reads compressed logs) |
| **Tests** | `make test` — 64 cases, no GPU required (the driver library still has to load) |

## Install

Download the latest package from
[**Releases**](https://github.com/ITUROBLAB/gpuwho/releases/latest) and install
it. The machine is collecting and ready to report as soon as it lands — there
is nothing further to configure:

```sh
sudo apt install ./gpuwho_0.1.1_amd64.deb
gpuwho report --day
```

The postinst enables accounting mode, enables and starts the collector timer,
and primes the state directory with one immediate tick, so `gpuwho report`
works right away rather than after the first minute. It warns on stderr if
accounting could not be enabled or if `nvidia-persistenced` is not running.

To build the package yourself instead:

```sh
make deb
sudo apt install ./gpuwho_0.1.1_amd64.deb
```

`apt remove` stops and disables the units but keeps your history;
`apt purge` also deletes `/var/lib/gpuwho` and the config.

## Build from source

```sh
make
sudo make install          # /usr/local/bin/gpuwho + man page
sudo make install-conf     # /etc/gpuwho/ignore.conf (never overwrites)
```

Needs a C11 compiler and `nvml.h`, which ships with the CUDA toolkit
(`cuda-nvml-dev`, `nvidia-cuda-dev`) or the driver development package. If it
lives somewhere unusual, point the build at it:

```sh
make CUDA_HOME=/opt/cuda
make config                # show what the build found
```

`libnvidia-ml` must match the running driver, so it is always linked
dynamically. `make config` prints the link target that was chosen.

## Snapshot

```
$ gpuwho
GPU 0  RTX 5090       28.4/32.0 GiB   91%
  yavuz   41235  21.0G  26:12:44  python train.py
  burak   50112   6.9G   0:41:03  python eval.py
GPU 1  RTX 5070 Ti     0.3/16.0 GiB    0%
  (idle)
```

Per GPU: index, name, used/total memory, instantaneous utilization. Per
process: user, pid, GPU memory, uptime, command.

```
--gpu N     only this GPU index (repeatable)
--json      machine-readable output
```

## What gets counted

**Only compute contexts.** `nvmlDeviceGetComputeRunningProcesses` returns just
those, so Xorg, the compositor, plasmashell, browsers and the rest of a desktop
session — all graphics-only, shown as `G` in `nvidia-smi` — never reach gpuwho
at all. No configuration is needed to exclude them; they are not there in the
first place.

The case that *does* need a decision is a process holding both a compute and a
graphics context (`C+G` in `nvidia-smi`): a desktop streaming host, a video
encoder, sometimes a browser doing CUDA decode. Those really are on the GPU, so
gpuwho counts them by default. If a particular one is not a job on your
machine, name it in `/etc/gpuwho/ignore.conf`:

```
sunshine            # command basename, or the full command line
*ffmpeg*            # globs work
cmd:*/some/path*    # full command line only
user:svc-stream     # by owner
uid:998             # by uid
```

The file ships **empty** on purpose: nothing is filtered until you say so, so
no one's GPU time is ever silently dropped from a report. The same rules can be
given ad hoc:

```sh
gpuwho --ignore sunshine        # hide it once
gpuwho --min-mem 100M           # hide trivial compute contexts
gpuwho --no-ignore              # ignore the rules; show every compute process
```

Rules apply to the snapshot *and* to the recorded history, so what you see is
what gets counted. Two details worth knowing:

- Filtering decides whether to **start** tracking. A process already being
  tracked is never dropped mid-run by a rule change or by memory drifting
  across `--min-mem`, so intervals always close cleanly instead of flapping.
- An ignored process still holds GPU memory. A card can read as `(idle)` in the
  snapshot while its memory is not free — `--no-ignore` shows the truth.

## Waiting for a GPU

Not a daemon: it runs in the foreground, polls, notifies once, and exits.

```sh
gpuwho wait --gpu 1                      # block until GPU 1 has no compute processes
gpuwho wait --any --mem-free 8G          # any GPU, idle and with 8 GiB free
gpuwho wait --gpu 0 --ntfy my-topic      # ping an ntfy topic when it frees up
gpuwho wait --gpu 0 --webhook https://…  # or POST a JSON body anywhere
```

With no `--gpu`, every GPU is selected and **all** of them must be free;
`--any` is satisfied as soon as one is. Notifications exec `curl` directly, so
the topic and URL are never passed through a shell.

Exit codes: `0` condition met, `1` error, `2` timed out (`--timeout`).

## History

History is optional and sits on top of the snapshot core. It needs three
things: the driver's accounting mode, a systemd timer running a one-shot
collector, and an append-only event log.

```sh
sudo gpuwho setup              # enable accounting mode, print install guidance
sudo make install-units
sudo systemctl daemon-reload
sudo systemctl enable --now gpuwho-accounting.service
sudo systemctl enable --now gpuwho-collect.timer
```

`gpuwho setup --check` reports the current state without changing anything.

Accounting mode is what supplies the per-process metrics. Enabling it needs
root; reading it does not. Both the mode and the accounting buffer are cleared
by a reboot or a driver reload, which is why `gpuwho-accounting.service` runs at
boot. Run `nvidia-persistenced` as well, so the kernel module stays resident
while the GPUs are idle and the setting survives.

### Report

```
$ gpuwho report --week
user      gpu-hours   avg-util   peak-mem   jobs
yavuz        41.2        83%      21.0G       6
burak        12.7        64%      11.5G       9
unknown       0.4          -          -       3
```

- `gpu-hours` is the sum of interval durations **per GPU** — one hour on two
  GPUs is two GPU-hours.
- `avg-util` is the duration-weighted mean of the accounting utilization.
- `peak-mem` is the maximum observed `max_mem_mb`; `jobs` is the interval count.
- Intervals whose end record carried no metrics contribute to `gpu-hours` and
  `jobs` but are excluded from `avg-util` and `peak-mem` (shown as `-`).

```
--day / --week / --month / --all      relative windows (default: --week)
--since WHEN / --until WHEN           explicit window
--json                                machine-readable output
```

`WHEN` is `now`, a relative age (`7d`, `12h`, `30m`, `2w`), an absolute
`YYYY-MM-DD[ HH:MM[:SS]]` in local time, or `@<epoch-seconds>`.

## How it works

```
        every 60 s (systemd timer)
 NVML ──┐
        ├─> gpuwho collect ──> state.json   (open intervals + last_tick)
 /proc ─┘         │
                  └──────────> events-YYYY-MM.jsonl  (start/end events)
                                        │
 gpuwho report <────────────────────────┘
```

State lives in `/var/lib/gpuwho/` (created by systemd's `StateDirectory`;
override with `--state-dir` or `$GPUWHO_STATE_DIR`).

A plain file cannot update a row in place, so a process lifecycle is two
append-only events — `start` and `end` — joined at read time on
`(gpu, pid, pst)`. `pid` alone is not enough because pids recycle within days,
and `gpu` is part of the key so a process on two GPUs yields two intervals.

```json
{"v":1,"ev":"start","t":1756172000,"gpu":0,"pid":41235,"pst":1756171143,"uid":1000,"user":"yavuz","cmd":"python train.py"}
{"v":1,"ev":"end","t":1756258700,"gpu":0,"pid":41235,"pst":1756171143,"end":1756258640,"dur_s":87497,"avg_util":83,"max_mem_mb":21504,"src":"acct"}
```

The `src` field says where the end data came from. `"acct"` means the record was
found in the accounting buffer, so `end = startTime + time` and the metrics are
present. `"tick"` means it was not found — buffer overflow, a reboot, or
accounting never enabled — so `end = last_tick` and there are no metrics.

`state.json` exists because the collector is a one-shot process with no memory
between ticks. It is rewritten atomically (temp file + `rename`), so an
interrupted tick cannot leave corrupt state behind. Reboots resolve themselves:
on the first tick after boot none of the open processes are live, so they all
close with `src:"tick"`.

Event logs are one file per month. Gzipped months are read transparently.
Retention is file deletion — there is no database and nothing to vacuum.

## Keeping the log small

The log grows by a couple of lines per job, so it stays small for a long time.
Past 10 MB (`--log-max-size`, `0` disables) gpuwho starts saying so:

- `gpuwho report` warns on **stderr**, so piped output stays clean.
- The collector warns too, but only when the log crosses a *fresh multiple* of
  the limit — otherwise a one-minute timer would repeat the same line into
  journald forever. The level is remembered in `state.json` and resets on its
  own once the log is pruned back down.

`gpuwho prune` is the delete side of that. It is irreversible — a deleted month
is gone from every future report — so nothing goes without `--yes` or an
answered prompt, and `--dry-run` shows the exact list first:

```sh
gpuwho prune --keep 6 --dry-run     # what would go, if I kept 6 months?
sudo gpuwho prune --keep 6          # prompts before deleting
sudo gpuwho prune --older-than 2026-01-01 --yes
sudo gpuwho prune --all --yes       # everything, current month included
```

Three details that keep it safe:

- `--older-than` deletes a month only when **all** of it predates the cutoff,
  so it never destroys data newer than you asked for. Month boundaries are UTC
  while a bare date is local midnight; near a boundary use `@<epoch>` to be
  exact.
- `--keep` and `--older-than` never touch the month in progress. Only `--all`
  does.
- Pruning takes the collector's lock, so a month cannot be deleted out from
  under a tick that is appending to it.

Without a terminal to confirm at and without `--yes`, `prune` refuses and exits
nonzero rather than guessing — which is what you want from cron.

## What the numbers mean

These caveats are real and worth knowing before you use a report to settle an
argument:

- **Docker.** Workloads launched through the Docker daemon appear as `root` on
  the host, so they are attributed to root rather than to a person. Rootless
  Podman does not have this problem: the process genuinely runs under the
  user's uid.
- **CUDA MPS.** Client processes aggregate under the MPS server's pid, so
  per-person attribution is lost.
- **Short-lived processes.** A process that starts and dies between two ticks
  gets its metrics from accounting, but its owner stays `unknown` — `/proc`
  could not be read while it was alive.
- **Two different utilizations.** The snapshot shows *instantaneous device*
  utilization. The report's `avg-util` comes from accounting and is the
  percentage of each process's lifetime during which kernels were executing.
  They are not the same number and are never mixed.
- **Buffer rotation.** The accounting buffer is circular. If many short-lived
  processes rotate it, some end records will be metric-less (`src:"tick"`).
  `gpuwho setup --check` prints the buffer size.

## Tests

```sh
make test
```

The suite drives `gpuwho report` against synthetic event logs, so the paths
that depend on accounting mode are covered without root and without a GPU.

## Extending: Slurm

Slurm integration is **not** in this repository and is not planned here, but
nothing in the design blocks it, and the seam is deliberate.

Attribution is assembled in exactly one place — where `gpuwho collect` builds a
`start` event, marked in [src/cmd_collect.c](src/cmd_collect.c). A Slurm layer
would read `/proc/<pid>/cgroup`, pull the `job_<id>` component out of the slurm
cgroup path, and write it as one extra field on that event.

The log format is already prepared for it:

- Every line carries a schema version `v`, so a reader can tell what it is
  looking at.
- The reader ignores fields it does not know, so adding `job` breaks neither an
  existing log nor an older `gpuwho` reading a newer one. This is the reason
  the event log is JSONL and not CSV.
- The join key `(gpu, pid, pst)` is independent of any job concept, so
  per-process history keeps working whether or not a job id is present.

On the report side this would become a grouping option (`--by-job`) alongside
the existing per-user aggregation, which is already just a key choice in the
aggregation loop.

## Scope

No web UI, no database, no Slurm integration, no multi-node aggregation, no
Windows/WSL, no MIG support. See **Extending: Slurm** for the seam left open.

## Contact 

Feel free to reach out to the maintainer k.yavuzkurt1@gmail.com if you have questions, suggestions, or want to contribute.

## License

TBD
