/* gpuwho wait -- block until a GPU is free, then notify.
 *
 * Not a daemon: this runs in the foreground, polls NVML, fires one
 * notification when the condition holds, and exits. */

#include "gpuwho.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_interrupted;

static void on_signal(int sig)
{
	(void)sig;
	g_interrupted = 1;
}

static void wait_usage(FILE *f)
{
	fprintf(f,
"usage: gpuwho wait [options]\n"
"\n"
"Poll until the selected GPUs are free, print what happened, optionally send\n"
"a notification, and exit 0.  With no --gpu, every GPU is selected, and by\n"
"default all of them must be free; --any is satisfied as soon as one is.\n"
"\n"
"options:\n"
"  -g, --gpu N        only this GPU index (repeatable)\n"
"      --any          satisfied when any selected GPU is free\n"
"      --mem-free SZ  also require this much free memory (e.g. 8G, 512M;\n"
"                     a bare number is MiB)\n"
"  -i, --interval S   seconds between polls (default 10)\n"
"  -t, --timeout S    give up after S seconds (default: wait forever)\n"
"      --ntfy TOPIC   notify an ntfy topic (or a full ntfy URL)\n"
"      --webhook URL  POST a JSON body to this URL\n"
"  -h, --help         this text\n"
"\n"
"exit: 0 condition met, 1 error, 2 timed out.\n");
}

static int gpu_selected(const int *sel, int nsel, unsigned int idx)
{
	int i;

	if (nsel == 0)
		return 1;
	for (i = 0; i < nsel; i++)
		if (sel[i] == (int)idx)
			return 1;
	return 0;
}

static int gpu_free(const gw_device *d, unsigned long long need_free)
{
	if (d->nprocs > 0)
		return 0;
	if (need_free) {
		unsigned long long avail;
		if (d->mem_total == GW_MEM_UNKNOWN || d->mem_used == GW_MEM_UNKNOWN)
			return 0;
		avail = d->mem_total - d->mem_used;
		if (avail < need_free)
			return 0;
	}
	return 1;
}

/* exec curl directly rather than going through a shell: the topic and URL come
 * from the command line and must never be reinterpreted. */
static int run_curl(char *const *args)
{
	pid_t pid;
	int   status;

	pid = fork();
	if (pid < 0) {
		gw_warn("fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execvp("curl", args);
		fprintf(stderr, "gpuwho: cannot exec curl: %s\n", strerror(errno));
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	if (WIFEXITED(status))
		gw_warn("curl exited %d", WEXITSTATUS(status));
	else
		gw_warn("curl died on signal %d", WTERMSIG(status));
	return -1;
}

static int notify_ntfy(const char *topic, const char *msg)
{
	char url[1024];

	if (strstr(topic, "://"))
		snprintf(url, sizeof(url), "%s", topic);
	else
		snprintf(url, sizeof(url), "https://ntfy.sh/%s", topic);

	{
		char *const args[] = { (char *)"curl", (char *)"-fsS",
			               (char *)"-m",    (char *)"15",
			               (char *)"-H",    (char *)"Title: gpuwho",
			               (char *)"-d",    (char *)msg,
			               url,             NULL };
		return run_curl(args);
	}
}

static int notify_webhook(const char *url, const char *json)
{
	char *const args[] = { (char *)"curl", (char *)"-fsS",
		               (char *)"-m",    (char *)"15",
		               (char *)"-H",    (char *)"Content-Type: application/json",
		               (char *)"-d",    (char *)json,
		               (char *)url,     NULL };

	return run_curl(args);
}

int gw_cmd_wait(int argc, char **argv)
{
	gw_snapshot snap;
	int        *sel;
	int         nsel = 0;
	int         any = 0;
	unsigned long long need_free = 0;
	long long   interval = 10, timeout = 0;
	const char *ntfy = NULL, *webhook = NULL;
	int         i, rc = 1;
	long long   started;
	char        hostname[256];

	sel = gw_malloc(sizeof(int) * (size_t)(argc + 1));

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		long long   v;

		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			wait_usage(stdout);
			free(sel);
			return 0;
		} else if (strcmp(a, "--any") == 0) {
			any = 1;
		} else if (strcmp(a, "-g") == 0 || strcmp(a, "--gpu") == 0) {
			if (i + 1 >= argc)
				gw_die("--gpu needs an index");
			if (gw_parse_ll(argv[++i], &v) != 0 || v < 0)
				gw_die("bad GPU index '%s'", argv[i]);
			sel[nsel++] = (int)v;
		} else if (strcmp(a, "--mem-free") == 0) {
			if (i + 1 >= argc)
				gw_die("--mem-free needs a size");
			if (gw_parse_size(argv[++i], &need_free) != 0)
				gw_die("bad size '%s' (try 8G, 512M)", argv[i]);
		} else if (strcmp(a, "-i") == 0 || strcmp(a, "--interval") == 0) {
			if (i + 1 >= argc)
				gw_die("--interval needs seconds");
			if (gw_parse_ll(argv[++i], &interval) != 0 || interval < 1)
				gw_die("bad interval '%s'", argv[i]);
		} else if (strcmp(a, "-t") == 0 || strcmp(a, "--timeout") == 0) {
			if (i + 1 >= argc)
				gw_die("--timeout needs seconds");
			if (gw_parse_ll(argv[++i], &timeout) != 0 || timeout < 0)
				gw_die("bad timeout '%s'", argv[i]);
		} else if (strcmp(a, "--ntfy") == 0) {
			if (i + 1 >= argc)
				gw_die("--ntfy needs a topic or URL");
			ntfy = argv[++i];
		} else if (strcmp(a, "--webhook") == 0) {
			if (i + 1 >= argc)
				gw_die("--webhook needs a URL");
			webhook = argv[++i];
		} else {
			free(sel);
			gw_die("unknown option '%s' (try 'gpuwho wait --help')", a);
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (gw_nvml_init() != 0) {
		gw_warn("%s", gw_nvml_err());
		free(sel);
		return 1;
	}

	if (gethostname(hostname, sizeof(hostname)) != 0)
		snprintf(hostname, sizeof(hostname), "this host");
	hostname[sizeof(hostname) - 1] = '\0';

	started = (long long)time(NULL);

	for (;;) {
		int nsatisfied = 0, ncandidates = 0;
		int first_free = -1;
		size_t j;

		if (gw_snapshot_take(&snap) != 0) {
			gw_warn("%s", gw_nvml_err());
			gw_nvml_fini();
			free(sel);
			return 1;
		}

		for (j = 0; j < snap.n; j++) {
			const gw_device *d = &snap.dev[j];
			if (!gpu_selected(sel, nsel, d->index))
				continue;
			ncandidates++;
			if (gpu_free(d, need_free)) {
				nsatisfied++;
				if (first_free < 0)
					first_free = (int)j;
			}
		}

		if (ncandidates == 0) {
			gw_warn("no such GPU");
			gw_snapshot_free(&snap);
			gw_nvml_fini();
			free(sel);
			return 1;
		}

		if (any ? (nsatisfied > 0) : (nsatisfied == ncandidates)) {
			const gw_device *d = &snap.dev[first_free];
			char             freebuf[32], msg[512];
			unsigned long long avail =
			        (d->mem_total == GW_MEM_UNKNOWN)
			                ? GW_MEM_UNKNOWN
			                : d->mem_total - d->mem_used;

			gw_fmt_mem(avail, freebuf, sizeof(freebuf));
			if (any || ncandidates == 1)
				snprintf(msg, sizeof(msg),
				         "GPU %u (%s) is free on %s -- %s available",
				         d->index, gw_short_name(d->name), hostname,
				         freebuf);
			else
				snprintf(msg, sizeof(msg),
				         "all %d selected GPUs are free on %s",
				         ncandidates, hostname);

			printf("%s\n", msg);
			fflush(stdout);

			rc = 0;
			if (ntfy && notify_ntfy(ntfy, msg) != 0)
				rc = 1;
			if (webhook) {
				char json[1024];
				snprintf(json, sizeof(json),
				         "{\"event\":\"gpu_free\",\"host\":\"%s\","
				         "\"gpu\":%u,\"message\":\"%s\"}",
				         hostname, d->index, msg);
				if (notify_webhook(webhook, json) != 0)
					rc = 1;
			}

			gw_snapshot_free(&snap);
			break;
		}

		gw_snapshot_free(&snap);

		if (g_interrupted) {
			gw_warn("interrupted");
			rc = 1;
			break;
		}
		if (timeout > 0 && (long long)time(NULL) - started >= timeout) {
			gw_warn("timed out after %llds", timeout);
			rc = 2;
			break;
		}

		sleep((unsigned int)interval);

		if (g_interrupted) {
			gw_warn("interrupted");
			rc = 1;
			break;
		}
	}

	gw_nvml_fini();
	free(sel);
	return rc;
}
