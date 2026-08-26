/* Minimal JSON reader/writer.
 *
 * The writer is a growable buffer with a container stack, so the collector can
 * emit event lines without hand-rolling comma placement.  The reader builds a
 * small value tree; it is used for state.json and for event log lines. */
#ifndef GPUWHO_JSON_H
#define GPUWHO_JSON_H

#include <stddef.h>

/* ------------------------------------------------------------------ reader */

enum {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUM,
	JSON_STR,
	JSON_ARR,
	JSON_OBJ
};

typedef struct json_val json_val;

json_val *json_parse(const char *text, char *err, size_t errlen);
void      json_free(json_val *v);

int json_type(const json_val *v);

const json_val *json_get(const json_val *obj, const char *key);
size_t          json_len(const json_val *arr);
const json_val *json_at(const json_val *arr, size_t i);

long long   json_int(const json_val *v, long long dflt);
double      json_num(const json_val *v, double dflt);
const char *json_str(const json_val *v, const char *dflt);
/* Copy a string field into a fixed buffer; dflt is used when absent. */
void json_strcpy(const json_val *obj, const char *key, char *dst, size_t n,
                 const char *dflt);

/* ------------------------------------------------------------------ writer */

#define JSON_MAXDEPTH 8

typedef struct {
	char  *buf;
	size_t len, cap;
	int    depth;
	int    first[JSON_MAXDEPTH];
	int    isobj[JSON_MAXDEPTH];
} json_buf;

void jb_init(json_buf *b);
void jb_free(json_buf *b);
void jb_reset(json_buf *b);

/* key is NULL inside an array, non-NULL inside an object. */
void jb_obj(json_buf *b, const char *key);
void jb_arr(json_buf *b, const char *key);
void jb_end(json_buf *b);
void jb_str(json_buf *b, const char *key, const char *val);
void jb_null(json_buf *b, const char *key);
void jb_int(json_buf *b, const char *key, long long val);
void jb_double(json_buf *b, const char *key, double val, int decimals);

#endif /* GPUWHO_JSON_H */
