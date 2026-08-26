/* NVML layer.
 *
 * We link against libnvidia-ml directly rather than parsing nvidia-smi output:
 * nvidia-smi is itself a frontend to this library, so parsing it would add a
 * subprocess per query plus a text format that drifts across driver versions.
 *
 * Prototypes are never hand-declared.  Whatever the installed nvml.h maps the
 * plain names to (nvmlDeviceGetComputeRunningProcesses -> _v2/_v3, ...) is what
 * gets used, so the struct layout always matches the header we compiled with. */

#include "gpuwho.h"

#include <nvml.h>
#include <stdlib.h>
#include <string.h>

#ifdef NVML_DEVICE_NAME_V2_BUFFER_SIZE
#define GW_NAME_BUF NVML_DEVICE_NAME_V2_BUFFER_SIZE
#else
#define GW_NAME_BUF NVML_DEVICE_NAME_BUFFER_SIZE
#endif

static int           g_inited;
static unsigned int  g_ndev;
static nvmlDevice_t *g_dev;
static char          g_err[256];

static void set_err(const char *what, nvmlReturn_t r)
{
	snprintf(g_err, sizeof(g_err), "%s: %s", what, nvmlErrorString(r));
}

const char *gw_nvml_err(void)
{
	return g_err[0] ? g_err : "unknown NVML error";
}

int gw_nvml_init(void)
{
	nvmlReturn_t r;
	unsigned int i;

	if (g_inited)
		return 0;

	r = nvmlInit();
	if (r != NVML_SUCCESS) {
		set_err("nvmlInit", r);
		return -1;
	}

	r = nvmlDeviceGetCount(&g_ndev);
	if (r != NVML_SUCCESS) {
		set_err("nvmlDeviceGetCount", r);
		nvmlShutdown();
		return -1;
	}

	g_dev = gw_malloc(sizeof(*g_dev) * (g_ndev ? g_ndev : 1));
	for (i = 0; i < g_ndev; i++) {
		r = nvmlDeviceGetHandleByIndex(i, &g_dev[i]);
		if (r != NVML_SUCCESS) {
			set_err("nvmlDeviceGetHandleByIndex", r);
			free(g_dev);
			g_dev = NULL;
			nvmlShutdown();
			return -1;
		}
	}

	g_inited = 1;
	return 0;
}

void gw_nvml_fini(void)
{
	if (!g_inited)
		return;
	free(g_dev);
	g_dev = NULL;
	nvmlShutdown();
	g_inited = 0;
}

int gw_device_count(unsigned int *n)
{
	if (!g_inited)
		return -1;
	*n = g_ndev;
	return 0;
}

/* Standard NVML sizing dance: ask with a small count; on
 * NVML_ERROR_INSUFFICIENT_SIZE the required count has been written back. */
static int read_procs(nvmlDevice_t dev, gw_proc **out, size_t *nout)
{
	nvmlProcessInfo_t *infos = NULL;
	unsigned int       cap = 0, count;
	int                attempt;

	*out = NULL;
	*nout = 0;

	for (attempt = 0; attempt < 8; attempt++) {
		nvmlReturn_t r;

		count = cap;
		r = nvmlDeviceGetComputeRunningProcesses(dev, &count,
		                                         cap ? infos : NULL);
		if (r == NVML_SUCCESS) {
			unsigned int i;
			if (count == 0) {
				free(infos);
				return 0;
			}
			*out = gw_malloc(sizeof(gw_proc) * count);
			for (i = 0; i < count; i++) {
				(*out)[i].pid = infos[i].pid;
				(*out)[i].used_mem = infos[i].usedGpuMemory;
			}
			*nout = count;
			free(infos);
			return 0;
		}
		if (r == NVML_ERROR_INSUFFICIENT_SIZE) {
			/* count now holds the number of entries needed; leave
			 * headroom for processes started since the query. */
			cap = count + 8;
			infos = gw_realloc(infos, sizeof(*infos) * cap);
			continue;
		}
		if (r == NVML_ERROR_NOT_SUPPORTED) {
			free(infos);
			return 0;
		}
		set_err("nvmlDeviceGetComputeRunningProcesses", r);
		free(infos);
		return -1;
	}

	free(infos);
	snprintf(g_err, sizeof(g_err),
	         "nvmlDeviceGetComputeRunningProcesses: process list kept growing");
	return -1;
}

int gw_snapshot_take(gw_snapshot *s)
{
	unsigned int i;

	s->dev = NULL;
	s->n = 0;

	if (!g_inited)
		return -1;

	s->dev = gw_malloc(sizeof(gw_device) * (g_ndev ? g_ndev : 1));
	memset(s->dev, 0, sizeof(gw_device) * (g_ndev ? g_ndev : 1));
	s->n = g_ndev;

	for (i = 0; i < g_ndev; i++) {
		gw_device      *d = &s->dev[i];
		nvmlMemory_t    mem;
		nvmlUtilization_t util;
		char            name[GW_NAME_BUF];
		nvmlReturn_t    r;

		d->index = i;

		r = nvmlDeviceGetName(g_dev[i], name, sizeof(name));
		if (r == NVML_SUCCESS)
			snprintf(d->name, sizeof(d->name), "%s", name);
		else
			snprintf(d->name, sizeof(d->name), "GPU %u", i);

		r = nvmlDeviceGetMemoryInfo(g_dev[i], &mem);
		if (r == NVML_SUCCESS) {
			d->mem_used = mem.used;
			d->mem_total = mem.total;
		} else {
			d->mem_used = GW_MEM_UNKNOWN;
			d->mem_total = GW_MEM_UNKNOWN;
		}

		r = nvmlDeviceGetUtilizationRates(g_dev[i], &util);
		d->util_gpu = (r == NVML_SUCCESS) ? util.gpu : GW_UTIL_UNKNOWN;

		if (read_procs(g_dev[i], &d->procs, &d->nprocs) != 0) {
			gw_snapshot_free(s);
			return -1;
		}
	}
	return 0;
}

void gw_snapshot_free(gw_snapshot *s)
{
	size_t i;

	if (!s->dev)
		return;
	for (i = 0; i < s->n; i++)
		free(s->dev[i].procs);
	free(s->dev);
	s->dev = NULL;
	s->n = 0;
}

/* ------------------------------------------------------------- accounting */

int gw_acct_lookup(unsigned int gpu, unsigned int pid, gw_acct *out)
{
	nvmlAccountingStats_t st;
	nvmlReturn_t          r;

	if (!g_inited || gpu >= g_ndev)
		return -1;

	r = nvmlDeviceGetAccountingStats(g_dev[gpu], pid, &st);
	if (r == NVML_ERROR_NOT_FOUND)
		return 0;
	if (r != NVML_SUCCESS) {
		set_err("nvmlDeviceGetAccountingStats", r);
		return -1;
	}

	out->start_time = (long long)(st.startTime / 1000000ULL);
	out->duration_ms = (long long)st.time;
	out->max_mem = (st.maxMemoryUsage == (unsigned long long)NVML_VALUE_NOT_AVAILABLE)
	                   ? GW_MEM_UNKNOWN
	                   : st.maxMemoryUsage;
	out->gpu_util = (st.gpuUtilization == (unsigned int)NVML_VALUE_NOT_AVAILABLE)
	                    ? GW_UTIL_UNKNOWN
	                    : st.gpuUtilization;
	out->is_running = (int)st.isRunning;
	return 1;
}

int gw_acct_mode(unsigned int gpu, int *enabled)
{
	nvmlEnableState_t mode;
	nvmlReturn_t      r;

	if (!g_inited || gpu >= g_ndev)
		return -1;

	r = nvmlDeviceGetAccountingMode(g_dev[gpu], &mode);
	if (r != NVML_SUCCESS) {
		set_err("nvmlDeviceGetAccountingMode", r);
		return -1;
	}
	*enabled = (mode == NVML_FEATURE_ENABLED);
	return 0;
}

int gw_acct_enable(unsigned int gpu)
{
	nvmlReturn_t r;

	if (!g_inited || gpu >= g_ndev)
		return -1;

	r = nvmlDeviceSetAccountingMode(g_dev[gpu], NVML_FEATURE_ENABLED);
	if (r != NVML_SUCCESS) {
		set_err("nvmlDeviceSetAccountingMode", r);
		return -1;
	}
	return 0;
}

int gw_acct_buffer_size(unsigned int gpu, unsigned int *size)
{
	nvmlReturn_t r;

	if (!g_inited || gpu >= g_ndev)
		return -1;

	r = nvmlDeviceGetAccountingBufferSize(g_dev[gpu], size);
	if (r != NVML_SUCCESS) {
		set_err("nvmlDeviceGetAccountingBufferSize", r);
		return -1;
	}
	return 0;
}
