/* Process filtering.
 *
 * NVML already draws the important line for us: nvmlDeviceGetComputeRunningProcesses
 * returns only *compute* contexts, so Xorg, the compositor, the browser and the
 * rest of a desktop session -- all graphics-only (G in nvidia-smi) -- never
 * reach gpuwho at all.  Nothing here is needed to exclude them.
 *
 * What this layer is for is the remainder: processes that genuinely hold a
 * compute context (C or C+G) but that an operator does not want counted as
 * jobs.  That is a local policy question, so it is a config file and a flag
 * rather than a built-in list, and it ships empty. */

#include "gpuwho.h"

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

#define GW_IGNORE_FILE "/etc/gpuwho/ignore.conf"

enum { RULE_GLOB, RULE_CMD, RULE_USER, RULE_UID };

typedef struct {
	int       kind;
	long long uid;
	char      pat[256];
} rule;

static rule              *g_rules;
static size_t             g_nrules, g_cap;
static int                g_disabled;
static int                g_loaded;
static char               g_file[512];
static unsigned long long g_min_mem;

static void add_rule(int kind, const char *pat, long long uid)
{
	if (g_nrules == g_cap) {
		g_cap = g_cap ? g_cap * 2 : 16;
		g_rules = gw_realloc(g_rules, g_cap * sizeof(*g_rules));
	}
	g_rules[g_nrules].kind = kind;
	g_rules[g_nrules].uid = uid;
	snprintf(g_rules[g_nrules].pat, sizeof(g_rules[g_nrules].pat), "%s",
	         pat ? pat : "");
	g_nrules++;
}

void gw_ignore_add(const char *text)
{
	char   buf[300];
	char  *s = buf;
	size_t len;

	snprintf(buf, sizeof(buf), "%s", text);

	while (*s && isspace((unsigned char)*s))
		s++;
	len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';

	if (*s == '\0' || *s == '#')
		return;

	if (strncmp(s, "user:", 5) == 0) {
		add_rule(RULE_USER, s + 5, -1);
	} else if (strncmp(s, "cmd:", 4) == 0) {
		add_rule(RULE_CMD, s + 4, -1);
	} else if (strncmp(s, "uid:", 4) == 0) {
		long long v;
		if (gw_parse_ll(s + 4, &v) != 0) {
			gw_warn("ignoring bad rule '%s'", s);
			return;
		}
		add_rule(RULE_UID, "", v);
	} else {
		add_rule(RULE_GLOB, s, -1);
	}
}

void gw_ignore_file(const char *path)
{
	snprintf(g_file, sizeof(g_file), "%s", path);
	g_loaded = 0;
}

void gw_ignore_disable(void)
{
	g_disabled = 1;
}

void gw_ignore_min_mem(unsigned long long bytes)
{
	g_min_mem = bytes;
}

static void load_once(void)
{
	FILE       *f;
	char        line[512];
	const char *path;

	if (g_loaded)
		return;
	g_loaded = 1;

	if (g_file[0]) {
		path = g_file;
	} else {
		const char *env = getenv("GPUWHO_IGNORE_FILE");
		path = (env && *env) ? env : GW_IGNORE_FILE;
	}

	f = fopen(path, "r");
	if (!f) {
		/* An absent config is the normal case: no rules, no filtering. */
		if (errno != ENOENT)
			gw_warn("cannot read %s: %s", path, strerror(errno));
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		gw_ignore_add(line);
	}
	fclose(f);
}

/* The basename of argv[0], which is what a bare rule like "sunshine" is
 * meant to match. */
static void argv0_base(const char *cmd, char *out, size_t n)
{
	const char *sp = strchr(cmd, ' ');
	size_t      len = sp ? (size_t)(sp - cmd) : strlen(cmd);
	const char *slash = memrchr(cmd, '/', len);
	const char *base = slash ? slash + 1 : cmd;
	size_t      blen = len - (size_t)(base - cmd);

	if (blen >= n)
		blen = n - 1;
	memcpy(out, base, blen);
	out[blen] = '\0';
}

int gw_ignored(const gw_procinfo *info, unsigned long long used_mem)
{
	char   base[256];
	size_t i;

	if (g_disabled)
		return 0;

	load_once();

	if (g_min_mem && used_mem != GW_MEM_UNKNOWN && used_mem < g_min_mem)
		return 1;

	if (g_nrules == 0)
		return 0;

	argv0_base(info->cmd, base, sizeof(base));

	for (i = 0; i < g_nrules; i++) {
		const rule *r = &g_rules[i];

		switch (r->kind) {
		case RULE_UID:
			if (info->uid == r->uid)
				return 1;
			break;
		case RULE_USER:
			if (fnmatch(r->pat, info->user, 0) == 0)
				return 1;
			break;
		case RULE_CMD:
			if (fnmatch(r->pat, info->cmd, 0) == 0)
				return 1;
			break;
		case RULE_GLOB:
		default:
			if (fnmatch(r->pat, base, 0) == 0 ||
			    fnmatch(r->pat, info->cmd, 0) == 0)
				return 1;
			break;
		}
	}
	return 0;
}

int gw_ignore_active(void)
{
	if (g_disabled)
		return 0;
	load_once();
	return (g_nrules > 0 || g_min_mem > 0);
}

void gw_ignore_free(void)
{
	free(g_rules);
	g_rules = NULL;
	g_nrules = g_cap = 0;
}
