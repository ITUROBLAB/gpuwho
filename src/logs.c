/* Event log files: enumeration, size accounting, and opening.
 *
 * The log is one file per month under the state directory, optionally gzipped.
 * Reports, `gpuwho prune` and the size check all want the same view of it, so
 * it lives here rather than inside any one command. */

#include "gpuwho.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define GW_LOG_LIMIT_DEFAULT (10LL * 1024 * 1024)

static long long g_limit = GW_LOG_LIMIT_DEFAULT;

void gw_log_limit_set(long long bytes)
{
	g_limit = bytes;
}

long long gw_log_limit(void)
{
	return g_limit;
}

/* events-YYYY-MM.jsonl or events-YYYY-MM.jsonl.gz */
static int parse_name(const char *name, gw_logfile *out)
{
	int y, m, n = 0;

	if (sscanf(name, "events-%4d-%2d.jsonl%n", &y, &m, &n) != 2 || n == 0)
		return -1;
	if (m < 1 || m > 12)
		return -1;
	if (name[n] == '\0')
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

static long long month_epoch(int year, int month)
{
	struct tm tm;
	time_t    t;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = year - 1900;
	tm.tm_mon = month - 1;
	tm.tm_mday = 1;
	t = timegm(&tm);
	return (t == (time_t)-1) ? 0 : (long long)t;
}

static int cmp_logfile(const void *a, const void *b)
{
	const gw_logfile *x = a, *y = b;

	if (x->year != y->year)
		return x->year - y->year;
	if (x->month != y->month)
		return x->month - y->month;
	return strcmp(x->name, y->name);
}

int gw_logs_list(gw_logfile **out, size_t *nout)
{
	const char    *dir = gw_state_dir();
	DIR           *d;
	struct dirent *de;
	gw_logfile    *files = NULL;
	size_t         n = 0, cap = 0;

	*out = NULL;
	*nout = 0;

	d = opendir(dir);
	if (!d)
		return -1;

	while ((de = readdir(d)) != NULL) {
		gw_logfile  lf;
		struct stat st;

		memset(&lf, 0, sizeof(lf));
		if (parse_name(de->d_name, &lf) != 0)
			continue;

		snprintf(lf.path, sizeof(lf.path), "%s/%s", dir, lf.name);
		lf.bytes = (stat(lf.path, &st) == 0) ? (long long)st.st_size : 0;
		lf.month_start = month_epoch(lf.year, lf.month);
		lf.month_end = (lf.month == 12) ? month_epoch(lf.year + 1, 1)
		                                : month_epoch(lf.year, lf.month + 1);

		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			files = gw_realloc(files, cap * sizeof(*files));
		}
		files[n++] = lf;
	}
	closedir(d);

	qsort(files, n, sizeof(*files), cmp_logfile);
	*out = files;
	*nout = n;
	return 0;
}

long long gw_logs_total_bytes(void)
{
	gw_logfile *files;
	size_t      n, i;
	long long   total = 0;

	if (gw_logs_list(&files, &n) != 0)
		return 0;
	for (i = 0; i < n; i++)
		total += files[i].bytes;
	free(files);
	return total;
}

/* Gzipped months go through gzip(1) rather than linking zlib: this path is
 * read rarely and never in a hot loop. */
FILE *gw_log_open(const gw_logfile *lf, int *is_pipe)
{
	char quoted[1500], cmd[1600];
	size_t o = 0;
	const char *p;

	*is_pipe = 0;
	if (!lf->gz)
		return fopen(lf->path, "r");

	/* Single-quote the path; the state dir can come from a flag. */
	if (strlen(lf->path) * 4 + 3 >= sizeof(quoted)) {
		gw_warn("%s: path too long to decompress", lf->path);
		return NULL;
	}
	quoted[o++] = '\'';
	for (p = lf->path; *p; p++) {
		if (*p == '\'') {
			memcpy(quoted + o, "'\\''", 4);
			o += 4;
		} else {
			quoted[o++] = *p;
		}
	}
	quoted[o++] = '\'';
	quoted[o] = '\0';

	snprintf(cmd, sizeof(cmd), "gzip -dc -- %s", quoted);
	*is_pipe = 1;
	return popen(cmd, "r");
}

void gw_log_size_message(char *buf, size_t n, long long total, long long limit)
{
	char t[32], l[32];

	gw_fmt_bytes((unsigned long long)total, t, sizeof(t));
	gw_fmt_bytes((unsigned long long)limit, l, sizeof(l));
	snprintf(buf, n,
	         "event log is %s, over the %s limit; "
	         "'gpuwho prune --help' trims it",
	         t, l);
}
