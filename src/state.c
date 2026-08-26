/* state.json -- the collector's memory between ticks.
 *
 * The collector is a one-shot process, so "have I seen this process before?"
 * has to be answered from disk.  last_tick bounds the end time of intervals
 * that were still open when the machine went down. */

#include "gpuwho.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void gw_state_init(gw_state *st)
{
	memset(st, 0, sizeof(*st));
}

void gw_state_free(gw_state *st)
{
	free(st->open);
	memset(st, 0, sizeof(*st));
}

void gw_state_add(gw_state *st, const gw_open *rec)
{
	if (st->n == st->cap) {
		st->cap = st->cap ? st->cap * 2 : 16;
		st->open = gw_realloc(st->open, st->cap * sizeof(*st->open));
	}
	st->open[st->n++] = *rec;
}

void gw_state_remove(gw_state *st, size_t i)
{
	if (i >= st->n)
		return;
	memmove(&st->open[i], &st->open[i + 1],
	        (st->n - i - 1) * sizeof(*st->open));
	st->n--;
}

static char *read_whole_file(const char *path, int *absent)
{
	FILE  *f;
	char  *buf = NULL;
	size_t len = 0, cap = 0;

	*absent = 0;
	f = fopen(path, "r");
	if (!f) {
		if (errno == ENOENT)
			*absent = 1;
		return NULL;
	}
	for (;;) {
		size_t got;
		if (len + 4096 + 1 > cap) {
			cap = cap ? cap * 2 : 8192;
			buf = gw_realloc(buf, cap);
		}
		got = fread(buf + len, 1, 4096, f);
		len += got;
		if (got < 4096)
			break;
	}
	fclose(f);
	if (!buf) {
		buf = gw_malloc(1);
		len = 0;
	}
	buf[len] = '\0';
	return buf;
}

int gw_state_load(gw_state *st, const char *path)
{
	char           err[128];
	char          *text;
	json_val      *root;
	const json_val *open;
	size_t         i, n;
	int            absent;

	gw_state_init(st);

	text = read_whole_file(path, &absent);
	if (!text)
		return absent ? 1 : -1;

	root = json_parse(text, err, sizeof(err));
	free(text);
	if (!root) {
		gw_warn("%s: %s", path, err);
		return -1;
	}
	if (json_type(root) != JSON_OBJ) {
		gw_warn("%s: not a JSON object", path);
		json_free(root);
		return -1;
	}

	st->last_tick = json_int(json_get(root, "last_tick"), 0);

	open = json_get(root, "open");
	n = json_len(open);
	for (i = 0; i < n; i++) {
		const json_val *o = json_at(open, i);
		gw_open         rec;

		memset(&rec, 0, sizeof(rec));
		rec.gpu = (int)json_int(json_get(o, "gpu"), -1);
		rec.pid = json_int(json_get(o, "pid"), -1);
		rec.pst = json_int(json_get(o, "pst"), 0);
		rec.t0 = json_int(json_get(o, "t0"), 0);
		rec.uid = json_int(json_get(o, "uid"), -1);
		json_strcpy(o, "user", rec.user, sizeof(rec.user), "unknown");
		json_strcpy(o, "cmd", rec.cmd, sizeof(rec.cmd), "");

		if (rec.gpu < 0 || rec.pid < 0)
			continue;
		/* Written by a version that did not record t0. */
		if (rec.t0 == 0)
			rec.t0 = rec.pst ? rec.pst : st->last_tick;
		gw_state_add(st, &rec);
	}

	json_free(root);
	return 0;
}

int gw_state_save(const gw_state *st, const char *path)
{
	char      tmp[600];
	json_buf  b;
	FILE     *f;
	int       fd;
	size_t    i;
	int       rc = -1;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	jb_init(&b);
	jb_obj(&b, NULL);
	jb_int(&b, "schema", GPUWHO_SCHEMA);
	jb_int(&b, "last_tick", st->last_tick);
	jb_arr(&b, "open");
	for (i = 0; i < st->n; i++) {
		const gw_open *r = &st->open[i];
		jb_obj(&b, NULL);
		jb_int(&b, "gpu", r->gpu);
		jb_int(&b, "pid", r->pid);
		jb_int(&b, "pst", r->pst);
		jb_int(&b, "t0", r->t0);
		jb_int(&b, "uid", r->uid);
		jb_str(&b, "user", r->user);
		jb_str(&b, "cmd", r->cmd);
		jb_end(&b);
	}
	jb_end(&b);
	jb_end(&b);

	/* Temp file + rename: an interrupted tick can never leave torn state. */
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		goto out;
	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		goto out;
	}
	if (fwrite(b.buf, 1, b.len, f) != b.len || fputc('\n', f) == EOF) {
		fclose(f);
		unlink(tmp);
		goto out;
	}
	if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
		fclose(f);
		unlink(tmp);
		goto out;
	}
	if (fclose(f) != 0) {
		unlink(tmp);
		goto out;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		goto out;
	}
	rc = 0;
out:
	jb_free(&b);
	return rc;
}
