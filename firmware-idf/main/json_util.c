/*
 * json_util.c - small, allocation free JSON helpers for the web API.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "json_util.h"

#include <stdio.h>
#include <string.h>

/* ---- writer ---------------------------------------------------------------------- */
static void put_char(json_writer_t *w, char ch)
{
    if (w->len + 1 < w->cap) {
        w->buf[w->len++] = ch;
        w->buf[w->len] = '\0';
    } else {
        w->overflow = true;
    }
}

static void put_raw(json_writer_t *w, const char *s)
{
    while (*s != '\0') {
        put_char(w, *s++);
    }
}

static void separator(json_writer_t *w)
{
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (w->depth > 0) {
        if (!w->first[w->depth - 1]) {
            put_char(w, ',');
        }
        w->first[w->depth - 1] = false;
    }
}

static void put_escaped(json_writer_t *w, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    put_char(w, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        switch (*p) {
        case '"':
            put_raw(w, "\\\"");
            break;
        case '\\':
            put_raw(w, "\\\\");
            break;
        case '\n':
            put_raw(w, "\\n");
            break;
        case '\r':
            put_raw(w, "\\r");
            break;
        case '\t':
            put_raw(w, "\\t");
            break;
        default:
            if (*p < 0x20) {
                put_raw(w, "\\u00");
                put_char(w, hex[*p >> 4]);
                put_char(w, hex[*p & 0x0F]);
            } else {
                put_char(w, (char)*p);
            }
        }
    }
    put_char(w, '"');
}

void jw_init(json_writer_t *w, char *buf, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    if (cap > 0) {
        buf[0] = '\0';
    } else {
        w->overflow = true;
    }
}

bool jw_ok(const json_writer_t *w)
{
    return !w->overflow && w->depth == 0 && !w->after_key && w->len > 0;
}

static void open_scope(json_writer_t *w, char ch)
{
    separator(w);
    put_char(w, ch);
    if (w->depth < JSON_WRITER_MAX_DEPTH) {
        w->first[w->depth++] = true;
    } else {
        w->overflow = true; /* too deep: refuse the document */
    }
}

static void close_scope(json_writer_t *w, char ch)
{
    if (w->depth == 0 || w->after_key) {
        w->overflow = true;
        return;
    }
    w->depth--;
    put_char(w, ch);
}

void jw_obj_begin(json_writer_t *w)
{
    open_scope(w, '{');
}

void jw_obj_end(json_writer_t *w)
{
    close_scope(w, '}');
}

void jw_arr_begin(json_writer_t *w)
{
    open_scope(w, '[');
}

void jw_arr_end(json_writer_t *w)
{
    close_scope(w, ']');
}

void jw_key(json_writer_t *w, const char *key)
{
    separator(w);
    put_escaped(w, key);
    put_char(w, ':');
    w->after_key = true;
}

void jw_str(json_writer_t *w, const char *s)
{
    separator(w);
    if (s == NULL) {
        put_raw(w, "null");
    } else {
        put_escaped(w, s);
    }
}

void jw_int(json_writer_t *w, long long v)
{
    char tmp[24];
    separator(w);
    snprintf(tmp, sizeof(tmp), "%lld", v);
    put_raw(w, tmp);
}

void jw_uint(json_writer_t *w, unsigned long long v)
{
    char tmp[24];
    separator(w);
    snprintf(tmp, sizeof(tmp), "%llu", v);
    put_raw(w, tmp);
}

void jw_bool(json_writer_t *w, bool v)
{
    separator(w);
    put_raw(w, v ? "true" : "false");
}

void jw_null(json_writer_t *w)
{
    separator(w);
    put_raw(w, "null");
}

void jw_fixed(json_writer_t *w, long long value, unsigned decimals)
{
    char tmp[32];
    unsigned long long scale = 1;
    for (unsigned i = 0; i < decimals && i < 9; i++) {
        scale *= 10u;
    }
    const bool neg = value < 0;
    const unsigned long long mag = neg ? (unsigned long long)(-(value + 1)) + 1u : (unsigned long long)value;
    separator(w);
    if (decimals == 0) {
        snprintf(tmp, sizeof(tmp), "%s%llu", neg ? "-" : "", mag);
    } else {
        snprintf(tmp, sizeof(tmp), "%s%llu.%0*llu", neg ? "-" : "", mag / scale, (int)decimals, mag % scale);
    }
    put_raw(w, tmp);
}

void jw_kv_str(json_writer_t *w, const char *key, const char *s)
{
    jw_key(w, key);
    jw_str(w, s);
}

void jw_kv_int(json_writer_t *w, const char *key, long long v)
{
    jw_key(w, key);
    jw_int(w, v);
}

void jw_kv_uint(json_writer_t *w, const char *key, unsigned long long v)
{
    jw_key(w, key);
    jw_uint(w, v);
}

void jw_kv_bool(json_writer_t *w, const char *key, bool v)
{
    jw_key(w, key);
    jw_bool(w, v);
}

void jw_kv_fixed(json_writer_t *w, const char *key, long long value, unsigned decimals)
{
    jw_key(w, key);
    jw_fixed(w, value, decimals);
}

/* ---- reader ---------------------------------------------------------------------- */
typedef struct {
    const char *p;
    const char *end;
} cursor_t;

static void skip_ws(cursor_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) {
        c->p++;
    }
}

static int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/* Appends one byte; sets *truncated once the output is full (out may be NULL). */
static void emit(char *out, size_t cap, size_t *n, bool *truncated, unsigned char byte)
{
    if (out != NULL && *n + 1 < cap) {
        out[*n] = (char)byte;
    } else {
        *truncated = true;
    }
    (*n)++;
}

/* Parses a JSON string at c->p (which must be '"'). Decodes escapes into out
 * (NUL terminated, at most cap - 1 bytes). Returns false if malformed. */
static bool parse_string(cursor_t *c, char *out, size_t cap, size_t *out_n, bool *truncated)
{
    size_t n = 0;
    bool trunc = false;
    if (c->p >= c->end || *c->p != '"') {
        return false;
    }
    c->p++;
    for (;;) {
        if (c->p >= c->end) {
            return false;
        }
        const unsigned char ch = (unsigned char)*c->p++;
        if (ch == '"') {
            break;
        }
        if (ch < 0x20) {
            return false; /* raw control characters are not allowed */
        }
        if (ch != '\\') {
            emit(out, cap, &n, &trunc, ch);
            continue;
        }
        if (c->p >= c->end) {
            return false;
        }
        const char esc = *c->p++;
        switch (esc) {
        case '"':
        case '\\':
        case '/':
            emit(out, cap, &n, &trunc, (unsigned char)esc);
            break;
        case 'b':
            emit(out, cap, &n, &trunc, '\b');
            break;
        case 'f':
            emit(out, cap, &n, &trunc, '\f');
            break;
        case 'n':
            emit(out, cap, &n, &trunc, '\n');
            break;
        case 'r':
            emit(out, cap, &n, &trunc, '\r');
            break;
        case 't':
            emit(out, cap, &n, &trunc, '\t');
            break;
        case 'u': {
            if (c->end - c->p < 4) {
                return false;
            }
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) {
                const int v = hex_val(c->p[i]);
                if (v < 0) {
                    return false;
                }
                cp = (cp << 4) | (unsigned)v;
            }
            c->p += 4;
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                return false; /* surrogate pairs are not supported */
            }
            if (cp < 0x80) {
                emit(out, cap, &n, &trunc, (unsigned char)cp);
            } else if (cp < 0x800) {
                emit(out, cap, &n, &trunc, (unsigned char)(0xC0 | (cp >> 6)));
                emit(out, cap, &n, &trunc, (unsigned char)(0x80 | (cp & 0x3F)));
            } else {
                emit(out, cap, &n, &trunc, (unsigned char)(0xE0 | (cp >> 12)));
                emit(out, cap, &n, &trunc, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
                emit(out, cap, &n, &trunc, (unsigned char)(0x80 | (cp & 0x3F)));
            }
            break;
        }
        default:
            return false;
        }
    }
    if (out != NULL && cap > 0) {
        out[(n < cap) ? n : cap - 1] = '\0';
    }
    if (out_n != NULL) {
        *out_n = n;
    }
    if (truncated != NULL) {
        *truncated = trunc;
    }
    return true;
}

static bool is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

static bool skip_number(cursor_t *c)
{
    if (c->p < c->end && *c->p == '-') {
        c->p++;
    }
    if (c->p >= c->end || !is_digit(*c->p)) {
        return false;
    }
    if (*c->p == '0') {
        c->p++;
    } else {
        while (c->p < c->end && is_digit(*c->p)) {
            c->p++;
        }
    }
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        if (c->p >= c->end || !is_digit(*c->p)) {
            return false;
        }
        while (c->p < c->end && is_digit(*c->p)) {
            c->p++;
        }
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-')) {
            c->p++;
        }
        if (c->p >= c->end || !is_digit(*c->p)) {
            return false;
        }
        while (c->p < c->end && is_digit(*c->p)) {
            c->p++;
        }
    }
    return true;
}

static bool skip_literal(cursor_t *c, const char *lit)
{
    const size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) {
        return false;
    }
    c->p += n;
    return true;
}

/* Scalar values only: nested objects / arrays make the document invalid. */
static bool skip_value(cursor_t *c)
{
    if (c->p >= c->end) {
        return false;
    }
    switch (*c->p) {
    case '"':
        return parse_string(c, NULL, 0, NULL, NULL);
    case 't':
        return skip_literal(c, "true");
    case 'f':
        return skip_literal(c, "false");
    case 'n':
        return skip_literal(c, "null");
    default:
        return skip_number(c);
    }
}

/* Validates the whole object. Returns -1 if invalid, 0 if `key` is absent (or
 * NULL), 1 if found; *value then points at the (last) value of `key`. */
static int find_field(const char *json, size_t len, const char *key, const char **value)
{
    cursor_t c = {json, json + len};
    int found = 0;
    const size_t key_len = (key != NULL) ? strlen(key) : 0;

    if (json == NULL) {
        return -1;
    }
    skip_ws(&c);
    if (c.p >= c.end || *c.p != '{') {
        return -1;
    }
    c.p++;
    skip_ws(&c);
    if (c.p < c.end && *c.p == '}') {
        c.p++;
    } else {
        for (;;) {
            char kbuf[64];
            size_t kn = 0;
            bool trunc = false;
            skip_ws(&c);
            if (!parse_string(&c, kbuf, sizeof(kbuf), &kn, &trunc)) {
                return -1;
            }
            skip_ws(&c);
            if (c.p >= c.end || *c.p != ':') {
                return -1;
            }
            c.p++;
            skip_ws(&c);
            const char *vstart = c.p;
            if (!skip_value(&c)) {
                return -1;
            }
            if (key != NULL && !trunc && kn == key_len && memcmp(kbuf, key, kn) == 0) {
                *value = vstart;
                found = 1;
            }
            skip_ws(&c);
            if (c.p < c.end && *c.p == ',') {
                c.p++;
                continue;
            }
            if (c.p < c.end && *c.p == '}') {
                c.p++;
                break;
            }
            return -1;
        }
    }
    skip_ws(&c);
    return (c.p == c.end) ? found : -1;
}

bool json_flat_valid(const char *json, size_t len)
{
    return find_field(json, len, NULL, NULL) >= 0;
}

json_field_result_t json_flat_get_string(const char *json, size_t len, const char *key, char *out,
                                         size_t out_len)
{
    const char *value = NULL;
    const int r = find_field(json, len, key, &value);
    if (r < 0) {
        return JSON_FIELD_INVALID;
    }
    if (r == 0) {
        return JSON_FIELD_ABSENT;
    }
    cursor_t c = {value, json + len};
    size_t n = 0;
    bool trunc = false;
    if (out == NULL || out_len == 0 || *value != '"' || !parse_string(&c, out, out_len, &n, &trunc) || trunc) {
        if (out != NULL && out_len > 0) {
            out[0] = '\0';
        }
        return JSON_FIELD_INVALID;
    }
    return JSON_FIELD_OK;
}

json_field_result_t json_flat_get_bool(const char *json, size_t len, const char *key, bool *out)
{
    const char *value = NULL;
    const int r = find_field(json, len, key, &value);
    if (r < 0) {
        return JSON_FIELD_INVALID;
    }
    if (r == 0) {
        return JSON_FIELD_ABSENT;
    }
    const size_t left = (size_t)((json + len) - value);
    if (left >= 4 && memcmp(value, "true", 4) == 0) {
        *out = true;
        return JSON_FIELD_OK;
    }
    if (left >= 5 && memcmp(value, "false", 5) == 0) {
        *out = false;
        return JSON_FIELD_OK;
    }
    return JSON_FIELD_INVALID;
}
