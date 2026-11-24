/*
 * json_util.h - small, allocation free JSON helpers for the web API.
 *
 *  - json_writer_t builds a JSON document into a caller-provided buffer with
 *    automatic comma handling and string escaping. It never writes past the
 *    buffer; on overflow `overflow` is set and the output must be discarded.
 *  - json_flat_* read a flat JSON object ({"key": "string" | number | true |
 *    false | null, ...}). Nested objects / arrays, malformed input and strings
 *    longer than the output buffer are rejected.
 *
 * Plain C99 without ESP-IDF dependencies so it is unit tested on the host
 * (test/host/test_json_util.c).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JSON_WRITER_MAX_DEPTH 8

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
    bool after_key;
    uint8_t depth;
    bool first[JSON_WRITER_MAX_DEPTH];
} json_writer_t;

void jw_init(json_writer_t *w, char *buf, size_t cap);
/* True if the document is complete and fitted in the buffer. */
bool jw_ok(const json_writer_t *w);

void jw_obj_begin(json_writer_t *w);
void jw_obj_end(json_writer_t *w);
void jw_arr_begin(json_writer_t *w);
void jw_arr_end(json_writer_t *w);
void jw_key(json_writer_t *w, const char *key);

void jw_str(json_writer_t *w, const char *s); /* NULL -> null */
void jw_int(json_writer_t *w, long long v);
void jw_uint(json_writer_t *w, unsigned long long v);
void jw_bool(json_writer_t *w, bool v);
void jw_null(json_writer_t *w);
/* Fixed point number: value / 10^decimals, e.g. jw_fixed(w, -125, 2) -> -1.25 */
void jw_fixed(json_writer_t *w, long long value, unsigned decimals);

/* key + value shorthands */
void jw_kv_str(json_writer_t *w, const char *key, const char *s);
void jw_kv_int(json_writer_t *w, const char *key, long long v);
void jw_kv_uint(json_writer_t *w, const char *key, unsigned long long v);
void jw_kv_bool(json_writer_t *w, const char *key, bool v);
void jw_kv_fixed(json_writer_t *w, const char *key, long long value, unsigned decimals);

/* ---- reading -------------------------------------------------------------------- */
typedef enum {
    JSON_FIELD_ABSENT = 0, /* object is valid, key not present */
    JSON_FIELD_OK,         /* value copied */
    JSON_FIELD_INVALID,    /* malformed object, wrong value type or value too long */
} json_field_result_t;

/* Validate that `json` is a flat object. */
bool json_flat_valid(const char *json, size_t len);

/* String value of `key` (unescaped, NUL terminated). */
json_field_result_t json_flat_get_string(const char *json, size_t len, const char *key, char *out,
                                         size_t out_len);

/* Boolean value of `key`. */
json_field_result_t json_flat_get_bool(const char *json, size_t len, const char *key, bool *out);

#ifdef __cplusplus
}
#endif
