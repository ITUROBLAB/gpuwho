#include "json.h"
#include "gpuwho.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================= reader */

struct json_val {
	int type;
	union {
		int    b;
		double num;
		char  *str;
		struct {
			json_val **v;
			size_t     n;
		} arr;
		struct {
			char     **k;
			json_val **v;
			size_t     n;
		} obj;
	} u;
};

typedef struct {
	const char *p;
	int         depth;
	char       *err;
	size_t      errlen;
	int         failed;
} jparse;

static json_val *parse_value(jparse *s);

static void fail(jparse *s, const char *msg)
{
	if (!s->failed) {
		s->failed = 1;
		if (s->err && s->errlen)
			snprintf(s->err, s->errlen, "%s", msg);
	}
}

static json_val *val_new(int type)
{
	json_val *v = gw_malloc(sizeof(*v));
	memset(v, 0, sizeof(*v));
	v->type = type;
	return v;
}

void json_free(json_val *v)
{
	size_t i;

	if (!v)
		return;
	switch (v->type) {
	case JSON_STR:
		free(v->u.str);
		break;
	case JSON_ARR:
		for (i = 0; i < v->u.arr.n; i++)
			json_free(v->u.arr.v[i]);
		free(v->u.arr.v);
		break;
	case JSON_OBJ:
		for (i = 0; i < v->u.obj.n; i++) {
			free(v->u.obj.k[i]);
			json_free(v->u.obj.v[i]);
		}
		free(v->u.obj.k);
		free(v->u.obj.v);
		break;
	default:
		break;
	}
	free(v);
}

static void skip_ws(jparse *s)
{
	while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')
		s->p++;
}

/* Append one code point as UTF-8. */
static void utf8_put(char **out, unsigned int cp)
{
	char *o = *out;

	if (cp < 0x80) {
		*o++ = (char)cp;
	} else if (cp < 0x800) {
		*o++ = (char)(0xC0 | (cp >> 6));
		*o++ = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		*o++ = (char)(0xE0 | (cp >> 12));
		*o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*o++ = (char)(0x80 | (cp & 0x3F));
	} else {
		*o++ = (char)(0xF0 | (cp >> 18));
		*o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
		*o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*o++ = (char)(0x80 | (cp & 0x3F));
	}
	*out = o;
}

static int hex4(const char *p, unsigned int *out)
{
	unsigned int v = 0;
	int          i;

	for (i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned int)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned int)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned int)(c - 'A' + 10);
		else
			return -1;
	}
	*out = v;
	return 0;
}

static char *parse_string_raw(jparse *s)
{
	const char *start;
	char       *out, *o;
	size_t      cap;

	if (*s->p != '"') {
		fail(s, "expected string");
		return NULL;
	}
	s->p++;
	start = s->p;

	/* An escape never expands to more bytes than it occupies, so the raw
	 * span length is a safe upper bound for the decoded string. */
	while (*s->p && *s->p != '"') {
		if (*s->p == '\\') {
			if (!s->p[1]) {
				fail(s, "truncated escape");
				return NULL;
			}
			s->p += 2;
		} else {
			s->p++;
		}
	}
	if (*s->p != '"') {
		fail(s, "unterminated string");
		return NULL;
	}

	cap = (size_t)(s->p - start) + 1;
	out = gw_malloc(cap + 4);
	o   = out;

	for (s->p = start; *s->p != '"';) {
		if (*s->p != '\\') {
			*o++ = *s->p++;
			continue;
		}
		s->p++;
		switch (*s->p) {
		case '"':  *o++ = '"';  s->p++; break;
		case '\\': *o++ = '\\'; s->p++; break;
		case '/':  *o++ = '/';  s->p++; break;
		case 'b':  *o++ = '\b'; s->p++; break;
		case 'f':  *o++ = '\f'; s->p++; break;
		case 'n':  *o++ = '\n'; s->p++; break;
		case 'r':  *o++ = '\r'; s->p++; break;
		case 't':  *o++ = '\t'; s->p++; break;
		case 'u': {
			unsigned int cp;
			if (hex4(s->p + 1, &cp) != 0) {
				fail(s, "bad \\u escape");
				free(out);
				return NULL;
			}
			s->p += 5;
			if (cp >= 0xD800 && cp <= 0xDBFF && s->p[0] == '\\' &&
			    s->p[1] == 'u') {
				unsigned int lo;
				if (hex4(s->p + 2, &lo) == 0 && lo >= 0xDC00 &&
				    lo <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) +
					     (lo - 0xDC00);
					s->p += 6;
				}
			}
			utf8_put(&o, cp);
			break;
		}
		default:
			fail(s, "bad escape");
			free(out);
			return NULL;
		}
	}
	s->p++; /* closing quote */
	*o = '\0';
	return out;
}

static json_val *parse_object(jparse *s)
{
	json_val *v = val_new(JSON_OBJ);
	size_t    cap = 0;

	s->p++; /* '{' */
	skip_ws(s);
	if (*s->p == '}') {
		s->p++;
		return v;
	}
	for (;;) {
		char     *key;
		json_val *child;

		skip_ws(s);
		key = parse_string_raw(s);
		if (!key)
			goto bad;
		skip_ws(s);
		if (*s->p != ':') {
			free(key);
			fail(s, "expected ':'");
			goto bad;
		}
		s->p++;
		child = parse_value(s);
		if (!child) {
			free(key);
			goto bad;
		}
		if (v->u.obj.n == cap) {
			cap = cap ? cap * 2 : 8;
			v->u.obj.k = gw_realloc(v->u.obj.k, cap * sizeof(char *));
			v->u.obj.v = gw_realloc(v->u.obj.v, cap * sizeof(json_val *));
		}
		v->u.obj.k[v->u.obj.n] = key;
		v->u.obj.v[v->u.obj.n] = child;
		v->u.obj.n++;

		skip_ws(s);
		if (*s->p == ',') {
			s->p++;
			continue;
		}
		if (*s->p == '}') {
			s->p++;
			return v;
		}
		fail(s, "expected ',' or '}'");
		goto bad;
	}
bad:
	json_free(v);
	return NULL;
}

static json_val *parse_array(jparse *s)
{
	json_val *v = val_new(JSON_ARR);
	size_t    cap = 0;

	s->p++; /* '[' */
	skip_ws(s);
	if (*s->p == ']') {
		s->p++;
		return v;
	}
	for (;;) {
		json_val *child = parse_value(s);
		if (!child)
			goto bad;
		if (v->u.arr.n == cap) {
			cap = cap ? cap * 2 : 8;
			v->u.arr.v = gw_realloc(v->u.arr.v, cap * sizeof(json_val *));
		}
		v->u.arr.v[v->u.arr.n++] = child;

		skip_ws(s);
		if (*s->p == ',') {
			s->p++;
			continue;
		}
		if (*s->p == ']') {
			s->p++;
			return v;
		}
		fail(s, "expected ',' or ']'");
		goto bad;
	}
bad:
	json_free(v);
	return NULL;
}

static json_val *parse_value(jparse *s)
{
	json_val *v;

	if (s->failed)
		return NULL;
	if (++s->depth > 32) {
		fail(s, "nesting too deep");
		s->depth--;
		return NULL;
	}
	skip_ws(s);

	switch (*s->p) {
	case '{':
		v = parse_object(s);
		break;
	case '[':
		v = parse_array(s);
		break;
	case '"': {
		char *str = parse_string_raw(s);
		if (!str) {
			v = NULL;
			break;
		}
		v = val_new(JSON_STR);
		v->u.str = str;
		break;
	}
	case 't':
		if (strncmp(s->p, "true", 4) != 0) {
			fail(s, "bad literal");
			v = NULL;
			break;
		}
		s->p += 4;
		v = val_new(JSON_BOOL);
		v->u.b = 1;
		break;
	case 'f':
		if (strncmp(s->p, "false", 5) != 0) {
			fail(s, "bad literal");
			v = NULL;
			break;
		}
		s->p += 5;
		v = val_new(JSON_BOOL);
		v->u.b = 0;
		break;
	case 'n':
		if (strncmp(s->p, "null", 4) != 0) {
			fail(s, "bad literal");
			v = NULL;
			break;
		}
		s->p += 4;
		v = val_new(JSON_NULL);
		break;
	default: {
		char *end;
		double d = strtod(s->p, &end);
		if (end == s->p) {
			fail(s, "unexpected character");
			v = NULL;
			break;
		}
		s->p = end;
		v = val_new(JSON_NUM);
		v->u.num = d;
		break;
	}
	}
	s->depth--;
	return v;
}

json_val *json_parse(const char *text, char *err, size_t errlen)
{
	jparse    s = { text, 0, err, errlen, 0 };
	json_val *v;

	if (err && errlen)
		err[0] = '\0';
	v = parse_value(&s);
	if (!v)
		return NULL;
	skip_ws(&s);
	if (*s.p != '\0') {
		fail(&s, "trailing garbage");
		json_free(v);
		return NULL;
	}
	return v;
}

int json_type(const json_val *v)
{
	return v ? v->type : JSON_NULL;
}

const json_val *json_get(const json_val *obj, const char *key)
{
	size_t i;

	if (!obj || obj->type != JSON_OBJ)
		return NULL;
	for (i = 0; i < obj->u.obj.n; i++)
		if (strcmp(obj->u.obj.k[i], key) == 0)
			return obj->u.obj.v[i];
	return NULL;
}

size_t json_len(const json_val *arr)
{
	if (!arr || arr->type != JSON_ARR)
		return 0;
	return arr->u.arr.n;
}

const json_val *json_at(const json_val *arr, size_t i)
{
	if (!arr || arr->type != JSON_ARR || i >= arr->u.arr.n)
		return NULL;
	return arr->u.arr.v[i];
}

double json_num(const json_val *v, double dflt)
{
	if (!v || v->type != JSON_NUM)
		return dflt;
	return v->u.num;
}

long long json_int(const json_val *v, long long dflt)
{
	if (!v || v->type != JSON_NUM)
		return dflt;
	return (long long)v->u.num;
}

const char *json_str(const json_val *v, const char *dflt)
{
	if (!v || v->type != JSON_STR)
		return dflt;
	return v->u.str;
}

void json_strcpy(const json_val *obj, const char *key, char *dst, size_t n,
                 const char *dflt)
{
	const char *s = json_str(json_get(obj, key), dflt);

	if (!s)
		s = "";
	snprintf(dst, n, "%s", s);
}

/* ================================================================= writer */

static void jb_reserve(json_buf *b, size_t extra)
{
	if (b->len + extra + 1 <= b->cap)
		return;
	while (b->len + extra + 1 > b->cap)
		b->cap = b->cap ? b->cap * 2 : 256;
	b->buf = gw_realloc(b->buf, b->cap);
}

static void jb_raw(json_buf *b, const char *s, size_t n)
{
	jb_reserve(b, n);
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = '\0';
}

static void jb_putc(json_buf *b, char c)
{
	jb_raw(b, &c, 1);
}

void jb_init(json_buf *b)
{
	memset(b, 0, sizeof(*b));
	jb_reserve(b, 0);
	b->buf[0] = '\0';
	b->first[0] = 1;
	b->isobj[0] = 0;
}

void jb_free(json_buf *b)
{
	free(b->buf);
	memset(b, 0, sizeof(*b));
}

void jb_reset(json_buf *b)
{
	b->len = 0;
	b->depth = 0;
	b->first[0] = 1;
	b->isobj[0] = 0;
	if (b->buf)
		b->buf[0] = '\0';
}

static void jb_escape(json_buf *b, const char *s)
{
	jb_putc(b, '"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '"':  jb_raw(b, "\\\"", 2); break;
		case '\\': jb_raw(b, "\\\\", 2); break;
		case '\n': jb_raw(b, "\\n", 2); break;
		case '\r': jb_raw(b, "\\r", 2); break;
		case '\t': jb_raw(b, "\\t", 2); break;
		default:
			if (c < 0x20 || c == 0x7f) {
				char esc[8];
				int  m = snprintf(esc, sizeof(esc), "\\u%04x", c);
				jb_raw(b, esc, (size_t)m);
			} else {
				jb_putc(b, (char)c);
			}
		}
	}
	jb_putc(b, '"');
}

/* Emit the separator and, inside an object, the key. */
static void jb_prefix(json_buf *b, const char *key)
{
	if (!b->first[b->depth])
		jb_putc(b, ',');
	b->first[b->depth] = 0;
	if (b->isobj[b->depth] && key) {
		jb_escape(b, key);
		jb_putc(b, ':');
	}
}

static void jb_push(json_buf *b, int isobj)
{
	if (b->depth + 1 >= JSON_MAXDEPTH)
		gw_die("json writer: nesting too deep");
	b->depth++;
	b->first[b->depth] = 1;
	b->isobj[b->depth] = isobj;
}

void jb_obj(json_buf *b, const char *key)
{
	jb_prefix(b, key);
	jb_putc(b, '{');
	jb_push(b, 1);
}

void jb_arr(json_buf *b, const char *key)
{
	jb_prefix(b, key);
	jb_putc(b, '[');
	jb_push(b, 0);
}

void jb_end(json_buf *b)
{
	if (b->depth <= 0)
		gw_die("json writer: unbalanced end");
	jb_putc(b, b->isobj[b->depth] ? '}' : ']');
	b->depth--;
}

void jb_str(json_buf *b, const char *key, const char *val)
{
	jb_prefix(b, key);
	jb_escape(b, val ? val : "");
}

void jb_null(json_buf *b, const char *key)
{
	jb_prefix(b, key);
	jb_raw(b, "null", 4);
}

void jb_int(json_buf *b, const char *key, long long val)
{
	char tmp[32];
	int  n;

	jb_prefix(b, key);
	n = snprintf(tmp, sizeof(tmp), "%lld", val);
	jb_raw(b, tmp, (size_t)n);
}

void jb_double(json_buf *b, const char *key, double val, int decimals)
{
	char tmp[64];
	int  n;

	jb_prefix(b, key);
	n = snprintf(tmp, sizeof(tmp), "%.*f", decimals, val);
	jb_raw(b, tmp, (size_t)n);
}
