/* gpuwho -- shared declarations */
#ifndef GPUWHO_H
#define GPUWHO_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

#define GPUWHO_VERSION   "0.1.0"
#define GPUWHO_SCHEMA    1
#define GPUWHO_STATE_DIR "/var/lib/gpuwho"

#define GW_MEM_UNKNOWN  ((unsigned long long)-1)
#define GW_UTIL_UNKNOWN ((unsigned int)-1)

#define GW_USER_MAX 64
#define GW_CMD_MAX  512

/* ------------------------------------------------------------------ util.c */

void  gw_die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void  gw_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void *gw_malloc(size_t n);
void *gw_realloc(void *p, size_t n);
char *gw_strdup(const char *s);

const char *gw_state_dir(void);
void        gw_set_state_dir(const char *dir);
int         gw_mkdir_p(const char *path);

void gw_set_no_color(int on);
int  gw_color(FILE *f);

/* "26:12:44" */
void gw_fmt_hms(long long secs, char *buf, size_t n);
/* "21.0G" / "812M" */
void gw_fmt_mem(unsigned long long bytes, char *buf, size_t n);
/* "28.4" (GiB, one decimal) */
void gw_fmt_gib(unsigned long long bytes, char *buf, size_t n);

/* "8G", "512M", bare number = MiB. Returns 0 on success. */
int gw_parse_size(const char *s, unsigned long long *out);
/* "now", "7d"/"12h"/"30m"/"2w" (ago), "YYYY-MM-DD[ HH:MM[:SS]]". 0 on success. */
int gw_parse_time(const char *s, time_t now, time_t *out);
/* "42" -> 42, rejects trailing junk. 0 on success. */
int gw_parse_ll(const char *s, long long *out);

/* Replace control characters with spaces, in place. */
void gw_sanitize(char *s);
/* Trim the vendor prefix off an NVML device name for display. */
const char *gw_short_name(const char *name);

/* ------------------------------------------------------------- nvml_layer.c */

typedef struct {
	unsigned int       pid;
	unsigned long long used_mem; /* bytes, or GW_MEM_UNKNOWN */
} gw_proc;

typedef struct {
	unsigned int       index;
	char               name[96];
	unsigned long long mem_used;
	unsigned long long mem_total;
	unsigned int       util_gpu; /* percent, or GW_UTIL_UNKNOWN */
	gw_proc           *procs;
	size_t             nprocs;
} gw_device;

typedef struct {
	gw_device *dev;
	size_t     n;
} gw_snapshot;

typedef struct {
	long long          start_time;  /* epoch seconds */
	long long          duration_ms; /* 0 while the process is still running */
	unsigned long long max_mem;     /* bytes, or GW_MEM_UNKNOWN */
	unsigned int       gpu_util;    /* percent, or GW_UTIL_UNKNOWN */
	int                is_running;
} gw_acct;

int         gw_nvml_init(void);
void        gw_nvml_fini(void);
const char *gw_nvml_err(void);

int  gw_device_count(unsigned int *n);
int  gw_snapshot_take(gw_snapshot *s);
void gw_snapshot_free(gw_snapshot *s);

/* 1 = found, 0 = not in the accounting buffer, -1 = error */
int gw_acct_lookup(unsigned int gpu, unsigned int pid, gw_acct *out);
int gw_acct_mode(unsigned int gpu, int *enabled);
int gw_acct_enable(unsigned int gpu);
int gw_acct_buffer_size(unsigned int gpu, unsigned int *size);

/* --------------------------------------------------------------- procinfo.c */

typedef struct {
	long long uid;
	char      user[GW_USER_MAX];
	char      cmd[GW_CMD_MAX];
	long long pst; /* process start time, epoch seconds */
} gw_procinfo;

/* 0 on success, -1 if the process is gone (out is filled with "unknown"). */
int  gw_proc_lookup(pid_t pid, gw_procinfo *out);
void gw_proc_unknown(gw_procinfo *out);

/* ----------------------------------------------------------------- filter.c
 *
 * Graphics-only processes (Xorg, the compositor, browsers) never appear here:
 * NVML's compute process list excludes them by construction.  These rules are
 * for processes that really do hold a compute context but should not count as
 * jobs on this particular machine. */

void gw_ignore_add(const char *rule);   /* "name", "cmd:glob", "user:x", "uid:n" */
void gw_ignore_file(const char *path);  /* default /etc/gpuwho/ignore.conf */
void gw_ignore_disable(void);           /* --no-ignore */
void gw_ignore_min_mem(unsigned long long bytes);
int  gw_ignored(const gw_procinfo *info, unsigned long long used_mem);
int  gw_ignore_active(void);
void gw_ignore_free(void);

/* ------------------------------------------------------------------ state.c */

typedef struct {
	int       gpu;
	long long pid;
	long long pst;
	long long t0; /* when the collector first saw it, epoch seconds */
	long long uid;
	char      user[GW_USER_MAX];
	char      cmd[GW_CMD_MAX];
} gw_open;

typedef struct {
	long long last_tick;
	gw_open  *open;
	size_t    n, cap;
} gw_state;

void gw_state_init(gw_state *st);
int  gw_state_load(gw_state *st, const char *path); /* 0 ok, 1 absent, -1 error */
int  gw_state_save(const gw_state *st, const char *path);
void gw_state_add(gw_state *st, const gw_open *rec);
void gw_state_remove(gw_state *st, size_t i);
void gw_state_free(gw_state *st);

/* ------------------------------------------------------------- subcommands */

int gw_cmd_snapshot(int argc, char **argv);
int gw_cmd_wait(int argc, char **argv);
int gw_cmd_collect(int argc, char **argv);
int gw_cmd_report(int argc, char **argv);
int gw_cmd_setup(int argc, char **argv);

#endif /* GPUWHO_H */
