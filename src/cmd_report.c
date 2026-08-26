/* gpuwho report -- aggregate the event log over a time window.
 *
 * Reads only the log, so nothing needs to be running when a report is
 * generated.  Start and end events are joined on (gpu, pid, pst): pid alone is
 * not enough because pids recycle within days, and gpu is part of the key
 * because a process on two GPUs produces two intervals -- one hour on two GPUs
 * is two GPU-hours. */

#include "gpuwho.h"
#include "json.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	int       gpu;
	long long pid;
	long long pst;

	long long t_start;
	long long t_end;
	int       have_start;
	int       have_end;
	long long dur_s;

	long long avg_util;   /* -1 when the end record carried no metrics */
	long long max_mem_mb; /* -1 likewise */
	char      user[GW_USER_MAX];
} interval;

typedef struct {
	interval *v;
	size_t    n, cap;
	long     *slot; /* hash -> index into v, -1 empty */
	size_t    nslot;
} ivltab;

typedef struct {
	char      user[GW_USER_MAX];
	double    gpu_secs;
	double    util_num; /* sum(util * dur) over metric-bearing intervals */
	double    util_den; /* sum(dur) over the same */
	long long peak_mem_mb;
	long long jobs;
} useragg;

static void report_usage(FILE *f)
{
	fprintf(f,
"usage: gpuwho report [options]\n"
"\n"
"Aggregate per-user GPU usage from the event log.  gpu-hours is the sum of\n"
"interval durations per GPU; avg-util is the duration-weighted mean of the\n"
"accounting utilization (percent of each process's lifetime during which\n"
"kernels were executing -- not the instantaneous figure the snapshot shows).\n"
"Intervals whose end record carried no metrics count toward gpu-hours and\n"
"jobs but are excluded from avg-util and peak-mem.\n"
"\n"
"window (default: the last 7 days):\n"
"  --day             the last 24 hours\n"
"  --week            the last 7 days\n"
"  --month           the last 30 days\n"
"  --all             everything still in the log\n"
"  --since WHEN      start of the window\n"
"  --until WHEN      end of the window (default: now)\n"
"\n"
"WHEN is 'now', a relative age such as 7d / 12h / 30m / 2w, an absolute\n"
"'YYYY-MM-DD[ HH:MM[:SS]]' in local time, or '@<epoch-seconds>'.\n"
"\n"
"options:\n"
"  --json            machine-readable output\n"
"  -h, --help        this text\n");
}

/* ------------------------------------------------------------ interval table */

static unsigned long long keyhash(int gpu, long long pid, long long pst)
{
	unsigned long long h = 1469598103934665603ULL;

	h = (h ^ (unsigned long long)(unsigned int)gpu) * 1099511628211ULL;
	h = (h ^ (unsigned long long)pid) * 1099511628211ULL;
	h = (h ^ (unsigned long long)pst) * 1099511628211ULL;
	return h;
}

static void tab_init(ivltab *t)
{
	size_t i;

	memset(t, 0, sizeof(*t));
	t->nslot = 1024;
	t->slot = gw_malloc(sizeof(long) * t->nslot);
	for (i = 0; i < t->nslot; i++)
		t->slot[i] = -1;
}

static void tab_free(ivltab *t)
{
	free(t->v);
	free(t->slot);
	memset(t, 0, sizeof(*t));
}

static void tab_rehash(ivltab *t)
{
	size_t i, newn = t->nslot * 2;
	long  *ns = gw_malloc(sizeof(long) * newn);

	for (i = 0; i < newn; i++)
		ns[i] = -1;
	for (i = 0; i < t->n; i++) {
		const interval *iv = &t->v[i];
		size_t          h = (size_t)(keyhash(iv->gpu, iv->pid, iv->pst) &
		                             (newn - 1));
		while (ns[h] != -1)
			h = (h + 1) & (newn - 1);
		ns[h] = (long)i;
	}
	free(t->slot);
	t->slot = ns;
	t->nslot = newn;
}

static interval *tab_get(ivltab *t, int gpu, long long pid, long long pst)
{
	size_t h;

	if ((t->n + 1) * 10 >= t->nslot * 7)
		tab_rehash(t);

	h = (size_t)(keyhash(gpu, pid, pst) & (t->nslot - 1));
	while (t->slot[h] != -1) {
		interval *iv = &t->v[t->slot[h]];
		if (iv->gpu == gpu && iv->pid == pid && iv->pst == pst)
			return iv;
		h = (h + 1) & (t->nslot - 1);
	}

	if (t->n == t->cap) {
		t->cap = t->cap ? t->cap * 2 : 256;
		t->v = gw_realloc(t->v, t->cap * sizeof(*t->v));
	}
	{
		interval *iv = &t->v[t->n];
		memset(iv, 0, sizeof(*iv));
		iv->gpu = gpu;
		iv->pid = pid;
		iv->pst = pst;
		iv->avg_util = -1;
		iv->max_mem_mb = -1;
		snprintf(iv->user, sizeof(iv->user), "unknown");
		t->slot[h] = (long)t->n;
		t->n++;
		return iv;
	}
}

/* --------------------------------------------------------------- log files */

typedef struct {
	char name[256];
	int  gz;
	int  year, month;
} logfile;

static int logfile_cmp(const void *a, const void *b)
{
	const logfile *x = a, *y = b;

	if (x->year != y->year)
		return x->year - y->year;
	return x->month - y->month;
}

/* events-YYYY-MM.jsonl or events-YYYY-MM.jsonl.gz */
static int parse_logname(const char *name, logfile *out)
{
	int y, m, n = 0;

	if (sscanf(name, "events-%4d-%2d.jsonl%n", &y, &m, &n) != 2 || n == 0)
		return -1;
	if (m < 1 || m > 12)
		return -1;
	if (strcmp(name + n, "") == 0)
		out->gz = 0;
	else if (strcmp(name + n, ".gz") == 0)
		out->gz = 1;
	else
		return -1;
	out->year = y;
	out->month = m;
	snprintf(out->name, sizeof(out->name), "%s", name);
	return 0;
}

static int shquote(const char *in, char *out, size_t n)
{
	size_t o = 0;

	if (n < 3)
		return -1;
	out[o++] = '\'';
	for (; *in; in++) {
		if (*in == '\'') {
			if (o + 4 >= n)
				return -1;
			memcpy(out + o, "'\\''", 4);
			o += 4;
		} else {
			if (o + 2 >= n)
				return -1;
			out[o++] = *in;
		}
	}
	out[o++] = '\'';
	out[o] = '\0';
	return 0;
}

/* Older months are gzipped; shelling out to gzip keeps zlib off the link
 * line for a path that is read rarely and never in the hot loop. */
static FILE *open_log(const char *dir, const logfile *lf, int *is_pipe)
{
	char path[700];

	snprintf(path, sizeof(path), "%s/%s", dir, lf->name);
	*is_pipe = 0;

	if (!lf->gz)
		return fopen(path, "r");

	{
		char quoted[1500], cmd[1600];
		if (shquote(path, quoted, sizeof(quoted)) != 0) {
			gw_warn("%s: path too long to decompress", path);
			return NULL;
		}
		snprintf(cmd, sizeof(cmd), "gzip -dc -- %s", quoted);
		*is_pipe = 1;
		return popen(cmd, "r");
	}
}

static void ingest_line(ivltab *tab, const char *line, const char *fname,
                        long long lineno)
{
	char            err[128];
	json_val       *v;
	const char     *ev;
	int             gpu;
	long long       pid, pst;
	interval       *iv;

	while (*line == ' ' || *line == '\t')
		line++;
	if (*line == '\0' || *line == '#')
		return;

	v = json_parse(line, err, sizeof(err));
	if (!v) {
		gw_warn("%s:%lld: %s", fname, lineno, err);
		return;
	}
	if (json_type(v) != JSON_OBJ) {
		json_free(v);
		return;
	}

	ev = json_str(json_get(v, "ev"), "");
	gpu = (int)json_int(json_get(v, "gpu"), -1);
	pid = json_int(json_get(v, "pid"), -1);
	pst = json_int(json_get(v, "pst"), 0);
	if (gpu < 0 || pid < 0) {
		json_free(v);
		return;
	}

	iv = tab_get(tab, gpu, pid, pst);

	if (strcmp(ev, "start") == 0) {
		long long t = json_int(json_get(v, "t"), 0);
		/* A tick that wrote its events and died before rewriting the
		 * state re-emits the start next time round; keep the first. */
		if (!iv->have_start || t < iv->t_start) {
			iv->t_start = t;
			iv->have_start = 1;
			json_strcpy(v, "user", iv->user, sizeof(iv->user),
			            "unknown");
		}
	} else if (strcmp(ev, "end") == 0) {
		if (!iv->have_end) {
			const json_val *util = json_get(v, "avg_util");
			const json_val *mem = json_get(v, "max_mem_mb");

			iv->t_end = json_int(json_get(v, "end"),
			                     json_int(json_get(v, "t"), 0));
			iv->dur_s = json_int(json_get(v, "dur_s"), 0);
			iv->have_end = 1;
			if (json_type(util) == JSON_NUM)
				iv->avg_util = json_int(util, -1);
			if (json_type(mem) == JSON_NUM)
				iv->max_mem_mb = json_int(mem, -1);
		}
	}

	json_free(v);
}

/* Read every month whose file could hold a record at or before `to`.  Long
 * intervals can start many months before the window they land in, so the
 * cheap-and-correct choice is to read what is there; retention is what keeps
 * the log small. */
static int load_events(ivltab *tab, long long to, int *nfiles)
{
	const char *dir = gw_state_dir();
	DIR        *d;
	struct dirent *de;
	logfile    *files = NULL;
	size_t      n = 0, cap = 0, i;

	*nfiles = 0;

	d = opendir(dir);
	if (!d) {
		if (errno == ENOENT) {
			gw_warn("%s does not exist -- has the collector ever run?",
			        dir);
			return -1;
		}
		gw_warn("cannot read %s: %s", dir, strerror(errno));
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		logfile lf;
		if (parse_logname(de->d_name, &lf) != 0)
			continue;
		{
			struct tm tm;
			time_t    month_start;
			memset(&tm, 0, sizeof(tm));
			tm.tm_year = lf.year - 1900;
			tm.tm_mon = lf.month - 1;
			tm.tm_mday = 1;
			month_start = timegm(&tm);
			if (month_start != (time_t)-1 && (long long)month_start > to)
				continue; /* entirely after the window */
		}
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			files = gw_realloc(files, cap * sizeof(*files));
		}
		files[n++] = lf;
	}
	closedir(d);

	qsort(files, n, sizeof(*files), logfile_cmp);

	for (i = 0; i < n; i++) {
		int     is_pipe;
		FILE   *f = open_log(dir, &files[i], &is_pipe);
		char   *line = NULL;
		size_t  cap2 = 0;
		ssize_t got;
		long long lineno = 0;

		if (!f) {
			gw_warn("cannot read %s/%s: %s", dir, files[i].name,
			        strerror(errno));
			continue;
		}
		while ((got = getline(&line, &cap2, f)) != -1) {
			if (got > 0 && line[got - 1] == '\n')
				line[got - 1] = '\0';
			ingest_line(tab, line, files[i].name, ++lineno);
		}
		free(line);
		if (is_pipe)
			pclose(f);
		else
			fclose(f);
		(*nfiles)++;
	}

	free(files);
	return 0;
}

/* -------------------------------------------------------------- aggregation */

static useragg *agg_find(useragg **arr, size_t *n, size_t *cap, const char *user)
{
	size_t i;

	for (i = 0; i < *n; i++)
		if (strcmp((*arr)[i].user, user) == 0)
			return &(*arr)[i];

	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 16;
		*arr = gw_realloc(*arr, *cap * sizeof(**arr));
	}
	{
		useragg *a = &(*arr)[(*n)++];
		memset(a, 0, sizeof(*a));
		snprintf(a->user, sizeof(a->user), "%s", user);
		a->peak_mem_mb = -1;
		return a;
	}
}

static int agg_cmp(const void *a, const void *b)
{
	const useragg *x = a, *y = b;

	if (x->gpu_secs > y->gpu_secs)
		return -1;
	if (x->gpu_secs < y->gpu_secs)
		return 1;
	return strcmp(x->user, y->user);
}

int gw_cmd_report(int argc, char **argv)
{
	long long from = 0, to = 0;
	long long now = (long long)time(NULL);
	int       have_since = 0, have_until = 0, window_set = 0;
	int       as_json = 0;
	int       i, nfiles = 0;
	ivltab    tab;
	useragg  *aggs = NULL;
	size_t    nagg = 0, aggcap = 0;
	size_t    k;
	int       w_user = 4;

	to = now;
	from = now - 7 * 86400; /* default window */

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		time_t      t;

		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			report_usage(stdout);
			return 0;
		} else if (strcmp(a, "--json") == 0) {
			as_json = 1;
		} else if (strcmp(a, "--day") == 0) {
			from = now - 86400;
			window_set = 1;
		} else if (strcmp(a, "--week") == 0) {
			from = now - 7 * 86400;
			window_set = 1;
		} else if (strcmp(a, "--month") == 0) {
			from = now - 30 * 86400;
			window_set = 1;
		} else if (strcmp(a, "--all") == 0) {
			from = 0;
			window_set = 1;
		} else if (strcmp(a, "--since") == 0) {
			if (i + 1 >= argc)
				gw_die("--since needs a time");
			if (gw_parse_time(argv[++i], (time_t)now, &t) != 0)
				gw_die("cannot parse time '%s'", argv[i]);
			from = (long long)t;
			have_since = 1;
		} else if (strcmp(a, "--until") == 0) {
			if (i + 1 >= argc)
				gw_die("--until needs a time");
			if (gw_parse_time(argv[++i], (time_t)now, &t) != 0)
				gw_die("cannot parse time '%s'", argv[i]);
			to = (long long)t;
			have_until = 1;
		} else {
			gw_die("unknown option '%s' (try 'gpuwho report --help')", a);
		}
	}
	(void)window_set;
	(void)have_since;
	(void)have_until;

	if (to < from)
		gw_die("empty window: --until is before --since");

	tab_init(&tab);
	if (load_events(&tab, to, &nfiles) != 0) {
		tab_free(&tab);
		return 1;
	}

	for (k = 0; k < tab.n; k++) {
		interval *iv = &tab.v[k];
		long long s, e, lo, hi, overlap;
		useragg  *a;

		if (iv->have_start) {
			s = iv->t_start;
		} else if (iv->have_end) {
			/* The start event has been rotated away; the end record
			 * still pins the interval down. */
			s = iv->t_end - iv->dur_s;
		} else {
			continue;
		}

		/* Still open: counted up to now. */
		e = iv->have_end ? iv->t_end : now;
		if (e < s)
			e = s;

		if (s > to || e < from)
			continue;

		lo = s > from ? s : from;
		hi = e < to ? e : to;
		overlap = hi > lo ? hi - lo : 0;

		a = agg_find(&aggs, &nagg, &aggcap, iv->user);
		a->gpu_secs += (double)overlap;
		a->jobs++;
		if (iv->avg_util >= 0) {
			a->util_num += (double)iv->avg_util * (double)overlap;
			a->util_den += (double)overlap;
		}
		if (iv->max_mem_mb >= 0 && iv->max_mem_mb > a->peak_mem_mb)
			a->peak_mem_mb = iv->max_mem_mb;
	}

	qsort(aggs, nagg, sizeof(*aggs), agg_cmp);

	for (k = 0; k < nagg; k++) {
		int len = (int)strlen(aggs[k].user);
		if (len > w_user)
			w_user = len;
	}

	if (as_json) {
		json_buf b;

		jb_init(&b);
		jb_obj(&b, NULL);
		jb_int(&b, "from", from);
		jb_int(&b, "to", to);
		jb_int(&b, "files", nfiles);
		jb_arr(&b, "users");
		for (k = 0; k < nagg; k++) {
			const useragg *a = &aggs[k];
			jb_obj(&b, NULL);
			jb_str(&b, "user", a->user);
			jb_double(&b, "gpu_hours", a->gpu_secs / 3600.0, 3);
			if (a->util_den > 0)
				jb_double(&b, "avg_util",
				          a->util_num / a->util_den, 1);
			else
				jb_null(&b, "avg_util");
			if (a->peak_mem_mb >= 0)
				jb_int(&b, "peak_mem_mb", a->peak_mem_mb);
			else
				jb_null(&b, "peak_mem_mb");
			jb_int(&b, "jobs", a->jobs);
			jb_end(&b);
		}
		jb_end(&b);
		jb_end(&b);
		printf("%s\n", b.buf);
		jb_free(&b);
	} else {
		printf("%-*s %11s %10s %10s %6s\n", w_user, "user", "gpu-hours",
		       "avg-util", "peak-mem", "jobs");
		for (k = 0; k < nagg; k++) {
			const useragg *a = &aggs[k];
			char           util[16], mem[16], hours[24];

			if (a->util_den > 0)
				snprintf(util, sizeof(util), "%.0f%%",
				         a->util_num / a->util_den);
			else
				snprintf(util, sizeof(util), "-");

			if (a->peak_mem_mb >= 0)
				gw_fmt_mem((unsigned long long)a->peak_mem_mb *
				                   1024ULL * 1024ULL,
				           mem, sizeof(mem));
			else
				snprintf(mem, sizeof(mem), "-");

			snprintf(hours, sizeof(hours), "%.1f", a->gpu_secs / 3600.0);
			printf("%-*s %11s %10s %10s %6lld\n", w_user, a->user,
			       hours, util, mem, a->jobs);
		}
		if (nagg == 0)
			fprintf(stderr,
			        "gpuwho: no usage recorded in this window%s\n",
			        nfiles ? "" : " (no event log found)");
	}

	free(aggs);
	tab_free(&tab);
	return 0;
}
