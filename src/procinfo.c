/* /proc layer.
 *
 * NVML hands us pids and nothing else; Linux supplies the owner, the command
 * line, and the process start time.  Every read here races with the process
 * exiting, so ENOENT is an expected outcome, never a fatal one. */

#include "gpuwho.h"

#include <errno.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long g_clk_tck;
static long long g_btime; /* boot time, epoch seconds; 0 = not yet read */

/* Boot time, from /proc/stat.  Read once per process. */
static long long boot_time(void)
{
	FILE *f;
	char  line[256];

	if (g_btime)
		return g_btime;

	f = fopen("/proc/stat", "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "btime ", 6) == 0) {
			g_btime = strtoll(line + 6, NULL, 10);
			break;
		}
	}
	fclose(f);
	return g_btime;
}

void gw_proc_unknown(gw_procinfo *out)
{
	memset(out, 0, sizeof(*out));
	out->uid = -1;
	snprintf(out->user, sizeof(out->user), "unknown");
	out->cmd[0] = '\0';
	out->pst = 0;
}

static int read_uid(pid_t pid, long long *uid)
{
	char  path[64];
	char  line[256];
	FILE *f;
	int   found = -1;

	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "Uid:", 4) == 0) {
			/* real, effective, saved, fs -- the first is the owner */
			*uid = strtoll(line + 4, NULL, 10);
			found = 0;
			break;
		}
	}
	fclose(f);
	return found;
}

static void read_cmd(pid_t pid, char *dst, size_t n)
{
	char   path[64];
	char   buf[GW_CMD_MAX];
	FILE  *f;
	size_t got, i;

	dst[0] = '\0';

	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	f = fopen(path, "r");
	if (f) {
		got = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		if (got > 0) {
			/* Arguments are NUL-separated; the last one may also be
			 * NUL-terminated, so drop trailing separators first. */
			while (got > 0 && buf[got - 1] == '\0')
				got--;
			for (i = 0; i < got; i++)
				if (buf[i] == '\0')
					buf[i] = ' ';
			buf[got] = '\0';
			if (got > 0) {
				snprintf(dst, n, "%s", buf);
				gw_sanitize(dst);
				return;
			}
		}
	}

	/* Kernel threads and some zombies have an empty cmdline; comm is all
	 * that is left, and it is truncated to 15 characters. */
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	f = fopen(path, "r");
	if (f) {
		if (fgets(buf, sizeof(buf), f)) {
			buf[strcspn(buf, "\n")] = '\0';
			snprintf(dst, n, "%s", buf);
			gw_sanitize(dst);
		}
		fclose(f);
	}
}

/* pst = btime + starttime / HZ, where starttime is field 22 of
 * /proc/<pid>/stat (jiffies since boot). */
static int read_pst(pid_t pid, long long *pst)
{
	char       path[64];
	char       buf[4096];
	FILE      *f;
	size_t     got;
	char      *p;
	int        field;
	long long  starttime = -1;
	long long  btime;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	got = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (got == 0)
		return -1;
	buf[got] = '\0';

	/* Field 2 (comm) is parenthesised and may itself contain spaces and
	 * parentheses, so scan from the LAST ')'.  The token right after it is
	 * field 3, which makes field 22 the 19th token from there. */
	p = strrchr(buf, ')');
	if (!p)
		return -1;
	p++;

	for (field = 3; *p; field++) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		if (field == 22) {
			starttime = strtoll(p, NULL, 10);
			break;
		}
		while (*p && *p != ' ')
			p++;
	}
	if (starttime < 0)
		return -1;

	if (!g_clk_tck) {
		g_clk_tck = sysconf(_SC_CLK_TCK);
		if (g_clk_tck <= 0)
			g_clk_tck = 100;
	}
	btime = boot_time();
	if (!btime)
		return -1;

	*pst = btime + starttime / g_clk_tck;
	return 0;
}

int gw_proc_lookup(pid_t pid, gw_procinfo *out)
{
	struct passwd  pw, *result = NULL;
	char           pwbuf[1024];
	long long      uid = -1;

	gw_proc_unknown(out);

	if (read_uid(pid, &uid) != 0)
		return -1; /* process is gone; caller keeps "unknown" */

	out->uid = uid;
	if (getpwuid_r((uid_t)uid, &pw, pwbuf, sizeof(pwbuf), &result) == 0 &&
	    result && result->pw_name)
		snprintf(out->user, sizeof(out->user), "%s", result->pw_name);
	else
		snprintf(out->user, sizeof(out->user), "%lld", uid);

	read_cmd(pid, out->cmd, sizeof(out->cmd));

	if (read_pst(pid, &out->pst) != 0)
		out->pst = 0;

	return 0;
}
