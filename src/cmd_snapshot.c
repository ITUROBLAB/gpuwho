/* gpuwho -- point-in-time snapshot of who is on the GPUs. */

#include "gpuwho.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_DIM   "\033[2m"
#define C_USER  "\033[36m"

typedef struct {
	unsigned int gpu;
	unsigned int pid;
	unsigned long long mem;
	gw_procinfo  info;
	long long    uptime;
} row;

static void snapshot_usage(FILE *f)
{
	fprintf(f,
"usage: gpuwho [options]\n"
"\n"
"Point-in-time view of every GPU: memory, utilization, and the compute\n"
"processes on it with their owner, uptime, and command line.\n"
"\n"
"options:\n"
"  --gpu N       only this GPU index (repeatable)\n"
"  --json        machine-readable output\n"
"  --no-color    never colorize\n"
"  -h, --help    this text\n");
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

static void print_json(const gw_snapshot *snap, const int *sel, int nsel,
                       long long now)
{
	json_buf b;
	size_t   i, j;

	jb_init(&b);
	jb_obj(&b, NULL);
	jb_int(&b, "t", now);
	jb_arr(&b, "gpus");
	for (i = 0; i < snap->n; i++) {
		const gw_device *d = &snap->dev[i];

		if (!gpu_selected(sel, nsel, d->index))
			continue;

		jb_obj(&b, NULL);
		jb_int(&b, "index", d->index);
		jb_str(&b, "name", d->name);
		if (d->mem_used == GW_MEM_UNKNOWN)
			jb_null(&b, "mem_used");
		else
			jb_int(&b, "mem_used", (long long)d->mem_used);
		if (d->mem_total == GW_MEM_UNKNOWN)
			jb_null(&b, "mem_total");
		else
			jb_int(&b, "mem_total", (long long)d->mem_total);
		if (d->util_gpu == GW_UTIL_UNKNOWN)
			jb_null(&b, "util");
		else
			jb_int(&b, "util", d->util_gpu);
		jb_arr(&b, "procs");
		for (j = 0; j < d->nprocs; j++) {
			gw_procinfo info;

			gw_proc_lookup((pid_t)d->procs[j].pid, &info);
			jb_obj(&b, NULL);
			jb_int(&b, "pid", d->procs[j].pid);
			jb_int(&b, "uid", info.uid);
			jb_str(&b, "user", info.user);
			if (d->procs[j].used_mem == GW_MEM_UNKNOWN)
				jb_null(&b, "mem");
			else
				jb_int(&b, "mem", (long long)d->procs[j].used_mem);
			jb_int(&b, "start", info.pst);
			jb_int(&b, "uptime_s", info.pst ? now - info.pst : 0);
			jb_str(&b, "cmd", info.cmd);
			jb_end(&b);
		}
		jb_end(&b);
		jb_end(&b);
	}
	jb_end(&b);
	jb_end(&b);

	printf("%s\n", b.buf);
	jb_free(&b);
}

static void print_text(const gw_snapshot *snap, const int *sel, int nsel,
                       long long now)
{
	row   *rows = NULL;
	size_t nrows = 0, cap = 0;
	size_t i, j;
	int    color = gw_color(stdout);
	int    w_user = 6, w_pid = 5, w_mem = 5, w_time = 8, w_name = 12;

	/* Collect first: column widths are computed across all GPUs so the
	 * process lines line up in one block. */
	for (i = 0; i < snap->n; i++) {
		const gw_device *d = &snap->dev[i];
		int              namelen;

		if (!gpu_selected(sel, nsel, d->index))
			continue;

		namelen = (int)strlen(gw_short_name(d->name));
		if (namelen > w_name)
			w_name = namelen;

		for (j = 0; j < d->nprocs; j++) {
			char tmp[32];
			row  r;
			int  len;

			memset(&r, 0, sizeof(r));
			r.gpu = d->index;
			r.pid = d->procs[j].pid;
			r.mem = d->procs[j].used_mem;
			gw_proc_lookup((pid_t)r.pid, &r.info);
			r.uptime = r.info.pst ? now - r.info.pst : -1;

			if (nrows == cap) {
				cap = cap ? cap * 2 : 16;
				rows = gw_realloc(rows, cap * sizeof(*rows));
			}
			rows[nrows++] = r;

			len = (int)strlen(r.info.user);
			if (len > w_user)
				w_user = len;
			len = snprintf(tmp, sizeof(tmp), "%u", r.pid);
			if (len > w_pid)
				w_pid = len;
			gw_fmt_mem(r.mem, tmp, sizeof(tmp));
			len = (int)strlen(tmp);
			if (len > w_mem)
				w_mem = len;
			if (r.uptime >= 0) {
				gw_fmt_hms(r.uptime, tmp, sizeof(tmp));
				len = (int)strlen(tmp);
				if (len > w_time)
					w_time = len;
			}
		}
	}

	for (i = 0; i < snap->n; i++) {
		const gw_device *d = &snap->dev[i];
		char             used[32], total[32], memfield[80];
		int              printed = 0;
		size_t           k;

		if (!gpu_selected(sel, nsel, d->index))
			continue;

		gw_fmt_gib(d->mem_used, used, sizeof(used));
		gw_fmt_gib(d->mem_total, total, sizeof(total));
		snprintf(memfield, sizeof(memfield), "%s/%s GiB", used, total);

		if (color)
			printf(C_BOLD "GPU %u  %-*s" C_RESET, d->index, w_name,
			       gw_short_name(d->name));
		else
			printf("GPU %u  %-*s", d->index, w_name,
			       gw_short_name(d->name));

		printf(" %13s", memfield);
		if (d->util_gpu == GW_UTIL_UNKNOWN)
			printf("     -\n");
		else
			printf("   %3u%%\n", d->util_gpu);

		for (k = 0; k < nrows; k++) {
			const row *r = &rows[k];
			char       mem[32], up[32];

			if (r->gpu != d->index)
				continue;

			gw_fmt_mem(r->mem, mem, sizeof(mem));
			if (r->uptime >= 0)
				gw_fmt_hms(r->uptime, up, sizeof(up));
			else
				snprintf(up, sizeof(up), "-");

			printf("  ");
			if (color)
				printf(C_USER "%-*s" C_RESET, w_user, r->info.user);
			else
				printf("%-*s", w_user, r->info.user);
			printf("  %*u  %*s  %*s  %s\n", w_pid, r->pid, w_mem, mem,
			       w_time, up,
			       r->info.cmd[0] ? r->info.cmd : "(unknown)");
			printed++;
		}

		if (!printed) {
			if (color)
				printf("  " C_DIM "(idle)" C_RESET "\n");
			else
				printf("  (idle)\n");
		}
	}

	free(rows);
}

int gw_cmd_snapshot(int argc, char **argv)
{
	gw_snapshot snap;
	int        *sel = NULL;
	int         nsel = 0;
	int         as_json = 0;
	int         i, rc = 0;
	long long   now;

	sel = gw_malloc(sizeof(int) * (size_t)(argc + 1));

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			snapshot_usage(stdout);
			free(sel);
			return 0;
		} else if (strcmp(a, "--json") == 0) {
			as_json = 1;
		} else if (strcmp(a, "--gpu") == 0 || strcmp(a, "-g") == 0) {
			long long v;
			if (i + 1 >= argc)
				gw_die("--gpu needs an index");
			if (gw_parse_ll(argv[++i], &v) != 0 || v < 0)
				gw_die("bad GPU index '%s'", argv[i]);
			sel[nsel++] = (int)v;
		} else {
			free(sel);
			gw_die("unknown option '%s' (try 'gpuwho --help')", a);
		}
	}

	if (gw_nvml_init() != 0) {
		free(sel);
		gw_warn("%s", gw_nvml_err());
		gw_warn("is the NVIDIA driver loaded? (try 'nvidia-smi')");
		return 1;
	}

	if (gw_snapshot_take(&snap) != 0) {
		gw_warn("%s", gw_nvml_err());
		gw_nvml_fini();
		free(sel);
		return 1;
	}

	now = (long long)time(NULL);
	if (as_json)
		print_json(&snap, sel, nsel, now);
	else
		print_text(&snap, sel, nsel, now);

	gw_snapshot_free(&snap);
	gw_nvml_fini();
	free(sel);
	return rc;
}
