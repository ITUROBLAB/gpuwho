#include "gpuwho.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_state_dir[512];
static int  g_no_color;

void gw_die(const char *fmt, ...)
{
	va_list ap;

	fputs("gpuwho: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

void gw_warn(const char *fmt, ...)
{
	va_list ap;

	fputs("gpuwho: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void *gw_malloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p)
		gw_die("out of memory");
	return p;
}

void *gw_realloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);

	if (!q)
		gw_die("out of memory");
	return q;
}

char *gw_strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char  *p = gw_malloc(n);

	memcpy(p, s, n);
	return p;
}

/* ---------------------------------------------------------------- paths */

const char *gw_state_dir(void)
{
	const char *env;

	if (g_state_dir[0])
		return g_state_dir;

	/* systemd sets STATE_DIRECTORY when StateDirectory= is used. */
	env = getenv("GPUWHO_STATE_DIR");
	if (!env || !*env)
		env = getenv("STATE_DIRECTORY");
	if (env && *env)
		snprintf(g_state_dir, sizeof(g_state_dir), "%s", env);
	else
		snprintf(g_state_dir, sizeof(g_state_dir), "%s", GPUWHO_STATE_DIR);
	return g_state_dir;
}

void gw_set_state_dir(const char *dir)
{
	snprintf(g_state_dir, sizeof(g_state_dir), "%s", dir);
}

int gw_mkdir_p(const char *path)
{
	char   tmp[512];
	size_t len;
	char  *p;

	len = (size_t)snprintf(tmp, sizeof(tmp), "%s", path);
	if (len >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	while (len > 1 && tmp[len - 1] == '/')
		tmp[--len] = '\0';

	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

/* ---------------------------------------------------------------- color */

void gw_set_no_color(int on)
{
	g_no_color = on;
}

int gw_color(FILE *f)
{
	const char *term;

	if (g_no_color)
		return 0;
	if (getenv("NO_COLOR"))
		return 0;
	term = getenv("TERM");
	if (term && strcmp(term, "dumb") == 0)
		return 0;
	return isatty(fileno(f));
}

/* ------------------------------------------------------------ formatting */

void gw_fmt_hms(long long secs, char *buf, size_t n)
{
	long long h, m, s;

	if (secs < 0)
		secs = 0;
	h = secs / 3600;
	m = (secs % 3600) / 60;
	s = secs % 60;
	snprintf(buf, n, "%lld:%02lld:%02lld", h, m, s);
}

void gw_fmt_mem(unsigned long long bytes, char *buf, size_t n)
{
	double gib;

	if (bytes == GW_MEM_UNKNOWN) {
		snprintf(buf, n, "-");
		return;
	}
	gib = (double)bytes / (1024.0 * 1024.0 * 1024.0);
	if (gib >= 1.0)
		snprintf(buf, n, "%.1fG", gib);
	else
		snprintf(buf, n, "%.0fM", (double)bytes / (1024.0 * 1024.0));
}

void gw_fmt_bytes(unsigned long long bytes, char *buf, size_t n)
{
	const double kib = 1024.0;
	const double mib = 1024.0 * 1024.0;
	const double gib = 1024.0 * 1024.0 * 1024.0;

	if (bytes == GW_MEM_UNKNOWN) {
		snprintf(buf, n, "-");
	} else if ((double)bytes >= gib) {
		snprintf(buf, n, "%.1fG", (double)bytes / gib);
	} else if ((double)bytes >= mib) {
		snprintf(buf, n, "%.1fM", (double)bytes / mib);
	} else if ((double)bytes >= kib) {
		snprintf(buf, n, "%.1fK", (double)bytes / kib);
	} else {
		snprintf(buf, n, "%lluB", bytes);
	}
}

void gw_fmt_gib(unsigned long long bytes, char *buf, size_t n)
{
	if (bytes == GW_MEM_UNKNOWN) {
		snprintf(buf, n, "-");
		return;
	}
	snprintf(buf, n, "%.1f", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

/* --------------------------------------------------------------- parsing */

int gw_parse_ll(const char *s, long long *out)
{
	char     *end;
	long long v;

	if (!s || !*s)
		return -1;
	errno = 0;
	v = strtoll(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0')
		return -1;
	*out = v;
	return 0;
}

int gw_parse_size(const char *s, unsigned long long *out)
{
	char  *end;
	double v;
	unsigned long long mult;

	if (!s || !*s)
		return -1;
	errno = 0;
	v = strtod(s, &end);
	if (end == s || v < 0 || errno == ERANGE)
		return -1;

	while (*end == ' ')
		end++;

	switch (tolower((unsigned char)*end)) {
	case '\0': mult = 1024ULL * 1024ULL; break; /* bare number = MiB */
	case 'k':  mult = 1024ULL; end++; break;
	case 'm':  mult = 1024ULL * 1024ULL; end++; break;
	case 'g':  mult = 1024ULL * 1024ULL * 1024ULL; end++; break;
	case 't':  mult = 1024ULL * 1024ULL * 1024ULL * 1024ULL; end++; break;
	default:   return -1;
	}
	/* Tolerate the "iB"/"B" tail of KiB, MB, GiB ... */
	if (tolower((unsigned char)*end) == 'i')
		end++;
	if (tolower((unsigned char)*end) == 'b')
		end++;
	if (*end != '\0')
		return -1;

	*out = (unsigned long long)(v * (double)mult);
	return 0;
}

int gw_parse_time(const char *s, time_t now, time_t *out)
{
	static const char *const fmts[] = {
		"%Y-%m-%dT%H:%M:%S", "%Y-%m-%d %H:%M:%S",
		"%Y-%m-%dT%H:%M",    "%Y-%m-%d %H:%M",
		"%Y-%m-%d",          NULL
	};
	const char *const *f;
	char              *end;
	long long          n;

	if (!s || !*s)
		return -1;

	if (strcmp(s, "now") == 0) {
		*out = now;
		return 0;
	}

	/* "@1756172000" -- epoch seconds, as date(1) accepts. */
	if (s[0] == '@') {
		long long v;
		if (gw_parse_ll(s + 1, &v) != 0)
			return -1;
		*out = (time_t)v;
		return 0;
	}

	/* Relative: "7d", "12h", "30m", "2w" -- that much time ago. */
	errno = 0;
	n = strtoll(s, &end, 10);
	if (end != s && errno == 0 && *end != '\0' && end[1] == '\0' && n >= 0) {
		long long mult;
		switch (*end) {
		case 's': mult = 1; break;
		case 'm': mult = 60; break;
		case 'h': mult = 3600; break;
		case 'd': mult = 86400; break;
		case 'w': mult = 7 * 86400; break;
		default:  mult = 0; break;
		}
		if (mult) {
			*out = now - (time_t)(n * mult);
			return 0;
		}
	}

	for (f = fmts; *f; f++) {
		struct tm tm;
		memset(&tm, 0, sizeof(tm));
		tm.tm_isdst = -1;
		if (strptime(s, *f, &tm)) {
			time_t t = mktime(&tm);
			if (t == (time_t)-1)
				return -1;
			*out = t;
			return 0;
		}
	}
	return -1;
}

void gw_sanitize(char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c < 0x20 || c == 0x7f)
			*s = ' ';
	}
}

const char *gw_short_name(const char *name)
{
	static const char *const prefixes[] = { "NVIDIA ", "GeForce ", "Tesla ",
		                                NULL };
	const char *const *p;
	int                changed = 1;

	while (changed) {
		changed = 0;
		for (p = prefixes; *p; p++) {
			size_t n = strlen(*p);
			if (strncmp(name, *p, n) == 0) {
				name += n;
				changed = 1;
			}
		}
	}
	return name;
}
