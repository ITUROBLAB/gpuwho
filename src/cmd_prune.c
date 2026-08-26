/* gpuwho prune -- delete event log files.
 *
 * Retention is file deletion; this is the command that does it.  The event log
 * is the only record of past usage and there is no undo, so nothing is removed
 * without either an explicit --yes or an answered prompt, and --dry-run shows
 * the exact list first. */

#include "gpuwho.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static void prune_usage(FILE *f)
{
	fprintf(f,
"usage: gpuwho prune (--older-than WHEN | --keep N | --all) [options]\n"
"\n"
"Delete monthly event log files.  This is irreversible: the event log is the\n"
"only record of past usage, and a deleted month is gone from every future\n"
"report.  Nothing is deleted without --yes or an answered prompt.\n"
"\n"
"what to delete (exactly one required):\n"
"  --older-than WHEN   months lying entirely before WHEN.  A month is kept\n"
"                      unless all of it is older, so nothing newer than you\n"
"                      asked for is ever destroyed.  Month boundaries are UTC\n"
"                      while a bare date is local midnight, so near a boundary\n"
"                      use '@<epoch>' if you need it exact.\n"
"  --keep N            all but the N most recent finished months\n"
"  --all               every event log, the month in progress included\n"
"\n"
"options:\n"
"  -n, --dry-run       list what would be deleted, delete nothing\n"
"  -y, --yes           do not prompt (required when stdin is not a terminal)\n"
"  -h, --help          this text\n"
"\n"
"WHEN is 'now', a relative age in days, hours, minutes or weeks ('90d', '2w'),\n"
"an absolute 'YYYY-MM-DD[ HH:MM[:SS]]', or '@<epoch-seconds>'.  There is no\n"
"month unit: write '6 months ago' as '180d' or as an absolute date.\n"
"\n"
"The current month is only ever a candidate under --all; --keep and\n"
"--older-than never touch a month that has not finished.\n");
}

int gw_cmd_prune(int argc, char **argv)
{
	gw_logfile *files = NULL;
	size_t      n = 0, i;
	int        *doomed = NULL;
	int         ndoomed = 0;
	long long   freed = 0;
	long long   now = (long long)time(NULL);
	long long   cutoff = 0;
	int         mode_older = 0, mode_keep = 0, mode_all = 0;
	long long   keep_n = 0;
	int         dry_run = 0, assume_yes = 0;
	int         j, rc = 0;
	char        lockpath[600];
	int         lockfd = -1;

	for (j = 1; j < argc; j++) {
		const char *a = argv[j];
		time_t      t;

		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			prune_usage(stdout);
			return 0;
		} else if (strcmp(a, "--older-than") == 0) {
			if (j + 1 >= argc)
				gw_die("--older-than needs a time");
			if (gw_parse_time(argv[++j], (time_t)now, &t) != 0)
				gw_die("cannot parse time '%s'", argv[j]);
			cutoff = (long long)t;
			mode_older = 1;
		} else if (strcmp(a, "--keep") == 0) {
			if (j + 1 >= argc)
				gw_die("--keep needs a count");
			if (gw_parse_ll(argv[++j], &keep_n) != 0 || keep_n < 0)
				gw_die("bad count '%s'", argv[j]);
			mode_keep = 1;
		} else if (strcmp(a, "--all") == 0) {
			mode_all = 1;
		} else if (strcmp(a, "-n") == 0 || strcmp(a, "--dry-run") == 0) {
			dry_run = 1;
		} else if (strcmp(a, "-y") == 0 || strcmp(a, "--yes") == 0) {
			assume_yes = 1;
		} else {
			gw_die("unknown option '%s' (try 'gpuwho prune --help')", a);
		}
	}

	if (mode_older + mode_keep + mode_all != 1) {
		prune_usage(stderr);
		return 1;
	}

	if (gw_logs_list(&files, &n) != 0) {
		gw_warn("cannot read %s: %s", gw_state_dir(), strerror(errno));
		return 1;
	}
	if (n == 0) {
		printf("no event logs in %s\n", gw_state_dir());
		free(files);
		return 0;
	}

	/* Hold the collector's lock so a month cannot be deleted out from under
	 * a tick that is midway through appending to it. */
	snprintf(lockpath, sizeof(lockpath), "%s/collect.lock", gw_state_dir());
	lockfd = open(lockpath, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (lockfd >= 0 && flock(lockfd, LOCK_EX) != 0) {
		close(lockfd);
		lockfd = -1;
	}

	doomed = gw_malloc(sizeof(int) * n);
	for (i = 0; i < n; i++)
		doomed[i] = 0;

	if (mode_all) {
		for (i = 0; i < n; i++)
			doomed[i] = 1;
	} else if (mode_older) {
		for (i = 0; i < n; i++) {
			/* Only whole months that have already ended. */
			if (files[i].month_end > 0 &&
			    files[i].month_end <= cutoff &&
			    files[i].month_end <= now)
				doomed[i] = 1;
		}
	} else { /* --keep N: files are sorted oldest first */
		long long keepable = 0;
		for (i = n; i-- > 0;) {
			if (files[i].month_end > now) {
				/* the month in progress is never counted or cut */
				continue;
			}
			if (keepable < keep_n)
				keepable++;
			else
				doomed[i] = 1;
		}
	}

	for (i = 0; i < n; i++) {
		if (doomed[i]) {
			ndoomed++;
			freed += files[i].bytes;
		}
	}

	if (ndoomed == 0) {
		printf("nothing to prune (%zu month%s kept)\n", n,
		       n == 1 ? "" : "s");
		goto out;
	}

	{
		char sz[32];
		printf("%s %d of %zu month%s:\n",
		       dry_run ? "would delete" : "about to delete", ndoomed, n,
		       n == 1 ? "" : "s");
		for (i = 0; i < n; i++) {
			if (!doomed[i])
				continue;
			gw_fmt_bytes((unsigned long long)files[i].bytes, sz,
			           sizeof(sz));
			printf("  %-30s %8s\n", files[i].name, sz);
		}
		gw_fmt_bytes((unsigned long long)freed, sz, sizeof(sz));
		printf("  %-30s %8s\n", "total", sz);
	}

	if (dry_run)
		goto out;

	if (!assume_yes) {
		char line[16];

		if (!isatty(STDIN_FILENO)) {
			/* Keep the refusal after the list when stdout is a pipe
			 * (block-buffered) and stderr is not. */
			fflush(stdout);
			gw_warn("refusing to delete without a terminal to confirm"
			        " at; pass --yes if you mean it");
			rc = 1;
			goto out;
		}
		printf("\nThis cannot be undone. Delete? [y/N] ");
		fflush(stdout);
		if (!fgets(line, sizeof(line), stdin) ||
		    (line[0] != 'y' && line[0] != 'Y')) {
			printf("aborted; nothing was deleted\n");
			goto out;
		}
	}

	freed = 0;
	for (i = 0; i < n; i++) {
		if (!doomed[i])
			continue;
		if (unlink(files[i].path) != 0) {
			gw_warn("cannot delete %s: %s", files[i].name,
			        strerror(errno));
			rc = 1;
			continue;
		}
		freed += files[i].bytes;
	}

	{
		char sz[32];
		gw_fmt_bytes((unsigned long long)freed, sz, sizeof(sz));
		printf("deleted, %s freed\n", sz);
	}

out:
	if (lockfd >= 0)
		close(lockfd);
	free(doomed);
	free(files);
	return rc;
}
