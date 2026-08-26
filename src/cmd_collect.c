/* gpuwho collect -- one tick of the collector, invoked by the systemd timer.
 *
 * A plain file cannot update a row in place, so a process lifecycle is stored
 * as two append-only events, `start` and `end`, joined at read time on
 * (gpu, pid, pst).  Everything the tick needs to remember between runs lives in
 * state.json.
 *
 * Reboots resolve themselves: on the first tick after boot none of the open
 * processes are live, so they all close with src:"tick" and end = last_tick,
 * preserving "started here, ended no later than that". */

#include "gpuwho.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* Two different clocks measure the process start time (NVML's accounting
 * timestamp and /proc's jiffies-since-boot), so they are matched with
 * tolerance rather than for equality. */
#define PST_TOLERANCE 5

typedef struct {
	unsigned int gpu;
	unsigned int pid;
	gw_procinfo  info;
	int          have_info;
} live_proc;

static void collect_usage(FILE *f)
{
	fprintf(f,
"usage: gpuwho collect [options]\n"
"\n"
"One collector tick: diff the live GPU process set against state.json,\n"
"append start/end events to the monthly event log, rewrite the state.\n"
"Prints nothing on success; errors go to stderr with a nonzero exit.\n"
"\n"
"options:\n"
"  -v, --verbose   print each event written\n"
"  -h, --help      this text\n");
}

/* Event log: one file per month, named in UTC so the boundaries do not move
 * with the local timezone.  Reports filter on timestamps, not on the name. */
static void event_path(char *buf, size_t n, long long now)
{
	time_t    t = (time_t)now;
	struct tm tm;

	gmtime_r(&t, &tm);
	snprintf(buf, n, "%s/events-%04d-%02d.jsonl", gw_state_dir(),
	         tm.tm_year + 1900, tm.tm_mon + 1);
}

static void events_write(FILE **f, long long now, const json_buf *b, int verbose)
{
	if (!*f) {
		char path[600];
		event_path(path, sizeof(path), now);
		*f = fopen(path, "a");
		if (!*f)
			gw_die("cannot open %s: %s", path, strerror(errno));
	}
	if (fprintf(*f, "%s\n", b->buf) < 0)
		gw_die("writing event log: %s", strerror(errno));
	if (verbose)
		printf("%s\n", b->buf);
}

static void emit_start(FILE **f, json_buf *b, long long now, const gw_open *r,
                       int verbose)
{
	jb_reset(b);
	jb_obj(b, NULL);
	jb_int(b, "v", GPUWHO_SCHEMA);
	jb_str(b, "ev", "start");
	jb_int(b, "t", now);
	jb_int(b, "gpu", r->gpu);
	jb_int(b, "pid", r->pid);
	jb_int(b, "pst", r->pst);
	jb_int(b, "uid", r->uid);
	jb_str(b, "user", r->user);
	jb_str(b, "cmd", r->cmd);
	jb_end(b);
	events_write(f, now, b, verbose);
}

static void emit_end(FILE **f, json_buf *b, long long now, long long last_tick,
                     const gw_open *r, int verbose)
{
	gw_acct   a;
	int       use_acct = 0;
	long long end, dur;

	if (gw_acct_lookup((unsigned int)r->gpu, (unsigned int)r->pid, &a) == 1) {
		/* `time` reads 0 while the process is alive.  A process can drop
		 * its GPU context and keep running, and pids get recycled --
		 * both have to fall through to the tick path. */
		int pst_ok = (r->pst == 0) ||
		             (llabs(a.start_time - r->pst) < PST_TOLERANCE);
		if (!a.is_running && a.duration_ms > 0 && pst_ok)
			use_acct = 1;
	}

	jb_reset(b);
	jb_obj(b, NULL);
	jb_int(b, "v", GPUWHO_SCHEMA);
	jb_str(b, "ev", "end");
	jb_int(b, "t", now);
	jb_int(b, "gpu", r->gpu);
	jb_int(b, "pid", r->pid);
	jb_int(b, "pst", r->pst);

	if (use_acct) {
		dur = a.duration_ms / 1000;
		end = a.start_time + dur;
		jb_int(b, "end", end);
		jb_int(b, "dur_s", dur);
		if (a.gpu_util != GW_UTIL_UNKNOWN)
			jb_int(b, "avg_util", a.gpu_util);
		if (a.max_mem != GW_MEM_UNKNOWN)
			jb_int(b, "max_mem_mb",
			       (long long)(a.max_mem / (1024ULL * 1024ULL)));
		jb_str(b, "src", "acct");
	} else {
		/* Not in the accounting buffer -- overflow, reboot, or
		 * accounting never enabled.  All we know is that it ended no
		 * later than the last tick, and we have no metrics. */
		end = last_tick > 0 ? last_tick : now;
		if (end < r->t0)
			end = r->t0;
		dur = end - r->t0;
		jb_int(b, "end", end);
		jb_int(b, "dur_s", dur);
		jb_str(b, "src", "tick");
	}
	jb_end(b);
	events_write(f, now, b, verbose);
}

/* Same process, or a recycled pid?  pst == 0 means /proc could not be read, in
 * which case the pid is all there is to match on. */
static int same_process(const gw_open *o, const live_proc *l)
{
	if (o->gpu != (int)l->gpu || o->pid != (long long)l->pid)
		return 0;
	if (o->pst == 0 || !l->have_info || l->info.pst == 0)
		return 1;
	return llabs(o->pst - l->info.pst) < PST_TOLERANCE;
}

static void gather_live(const gw_snapshot *snap, live_proc **out, size_t *nout)
{
	live_proc *live = NULL;
	size_t     n = 0, cap = 0;
	size_t     i, p;

	for (i = 0; i < snap->n; i++) {
		const gw_device *d = &snap->dev[i];

		for (p = 0; p < d->nprocs; p++) {
			live_proc lp;

			memset(&lp, 0, sizeof(lp));
			lp.gpu = d->index;
			lp.pid = d->procs[p].pid;
			/* A process that exits between the NVML query and this
			 * read degrades to "unknown"; it never aborts the tick. */
			lp.have_info = (gw_proc_lookup((pid_t)lp.pid, &lp.info) == 0);

			if (n == cap) {
				cap = cap ? cap * 2 : 16;
				live = gw_realloc(live, cap * sizeof(*live));
			}
			live[n++] = lp;
		}
	}
	*out = live;
	*nout = n;
}

int gw_cmd_collect(int argc, char **argv)
{
	char        lockpath[600], statepath[600];
	int         lockfd;
	int         verbose = 0;
	int         i, rc = 0;
	long long   now, last_tick;
	gw_state    st;
	gw_snapshot snap;
	live_proc  *live = NULL;
	size_t      nlive = 0;
	int        *seen = NULL;
	size_t      nloaded;
	FILE       *ev = NULL;
	json_buf    b;
	size_t      j, k;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			collect_usage(stdout);
			return 0;
		} else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
			verbose = 1;
		} else {
			gw_die("unknown option '%s' (try 'gpuwho collect --help')", a);
		}
	}

	if (gw_mkdir_p(gw_state_dir()) != 0)
		gw_die("cannot create %s: %s", gw_state_dir(), strerror(errno));

	/* The timer already serializes ticks; the lock is cheap insurance
	 * against one tick running long. */
	snprintf(lockpath, sizeof(lockpath), "%s/collect.lock", gw_state_dir());
	lockfd = open(lockpath, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (lockfd < 0)
		gw_die("cannot open %s: %s", lockpath, strerror(errno));
	if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
		if (errno == EWOULDBLOCK) {
			gw_warn("another collector tick is running; skipping");
			close(lockfd);
			return 0;
		}
		gw_die("flock %s: %s", lockpath, strerror(errno));
	}

	now = (long long)time(NULL);

	snprintf(statepath, sizeof(statepath), "%s/state.json", gw_state_dir());
	if (gw_state_load(&st, statepath) < 0) {
		/* Unreadable state: start clean rather than wedge every tick
		 * from here on.  Intervals open at that moment are lost. */
		gw_warn("%s unreadable; starting from an empty state", statepath);
		gw_state_init(&st);
	}
	last_tick = st.last_tick;

	if (gw_nvml_init() != 0) {
		/* Mid driver upgrade, say.  journald records it and the next
		 * tick retries cleanly. */
		gw_warn("%s", gw_nvml_err());
		gw_state_free(&st);
		close(lockfd);
		return 1;
	}
	if (gw_snapshot_take(&snap) != 0) {
		gw_warn("%s", gw_nvml_err());
		gw_nvml_fini();
		gw_state_free(&st);
		close(lockfd);
		return 1;
	}

	gather_live(&snap, &live, &nlive);

	jb_init(&b);
	nloaded = st.n;
	seen = nloaded ? gw_malloc(sizeof(int) * nloaded) : NULL;
	for (k = 0; k < nloaded; k++)
		seen[k] = 0;

	/* New processes: live but not in `open`. */
	for (j = 0; j < nlive; j++) {
		gw_open rec;
		int     matched = 0;

		for (k = 0; k < nloaded; k++) {
			if (seen[k])
				continue;
			if (same_process(&st.open[k], &live[j])) {
				seen[k] = 1;
				matched = 1;
				break;
			}
		}
		if (matched)
			continue;

		/* Filter only here, when deciding whether to start tracking.
		 * Already-open intervals are matched against the unfiltered live
		 * set above, so a process whose memory drifts across --min-mem
		 * cannot flap open and closed tick after tick: once tracked, it
		 * is tracked through to its end. */
		if (gw_ignored(&live[j].info, 0))
			continue;

		/* Attribution seam: everything a start event knows about a
		 * process is assembled here.  A Slurm integration would add its
		 * job id at this point -- read /proc/<pid>/cgroup, pull the
		 * job_<id> component out of the slurm cgroup path, and write it
		 * as an extra field.  Readers ignore unknown fields and every
		 * line carries "v", so adding one does not break an existing
		 * log or an older gpuwho reading it. */
		memset(&rec, 0, sizeof(rec));
		rec.gpu = (int)live[j].gpu;
		rec.pid = (long long)live[j].pid;
		rec.pst = live[j].info.pst;
		rec.t0 = now;
		rec.uid = live[j].info.uid;
		snprintf(rec.user, sizeof(rec.user), "%s", live[j].info.user);
		snprintf(rec.cmd, sizeof(rec.cmd), "%s", live[j].info.cmd);

		emit_start(&ev, &b, now, &rec, verbose);
		gw_state_add(&st, &rec);
	}

	/* Disappeared processes: in `open` but no longer live.  Walk the
	 * loaded prefix backwards so a removal cannot disturb an index still
	 * to be visited; records appended just above are live by construction
	 * and sit past that prefix. */
	for (k = nloaded; k-- > 0;) {
		if (seen[k])
			continue;
		emit_end(&ev, &b, now, last_tick, &st.open[k], verbose);
		gw_state_remove(&st, k);
	}

	if (ev) {
		/* One fsync per tick that produced events; ticks are a minute
		 * apart, so the cost is irrelevant and a power cut cannot eat
		 * an interval boundary. */
		if (fflush(ev) != 0 || fsync(fileno(ev)) != 0)
			gw_warn("syncing event log: %s", strerror(errno));
		if (fclose(ev) != 0) {
			gw_warn("closing event log: %s", strerror(errno));
			rc = 1;
		}
		ev = NULL;
	}

	/* Complain about an oversized log, but only when it crosses a fresh
	 * multiple of the limit.  A one-minute timer would otherwise repeat the
	 * same line into journald forever.  The counter resets on its own once
	 * the log is pruned back under the limit. */
	{
		long long limit = gw_log_limit();

		if (limit > 0) {
			long long total = gw_logs_total_bytes();
			long long step = total / limit;

			if (step > st.log_warned) {
				char msg[256];
				gw_log_size_message(msg, sizeof(msg), total, limit);
				gw_warn("%s", msg);
				st.log_warned = step;
			} else if (step < st.log_warned) {
				st.log_warned = step;
			}
		}
	}

	st.last_tick = now;
	if (gw_state_save(&st, statepath) != 0) {
		gw_warn("cannot write %s: %s", statepath, strerror(errno));
		rc = 1;
	}

	jb_free(&b);
	free(seen);
	free(live);
	gw_snapshot_free(&snap);
	gw_nvml_fini();
	gw_state_free(&st);
	close(lockfd);
	return rc;
}
