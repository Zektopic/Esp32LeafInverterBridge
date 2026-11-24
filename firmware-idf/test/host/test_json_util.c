/*
 * Host unit tests of main/json_util.c (run with ASan/UBSan, see CMakeLists.txt).
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "json_util.h"

static int checks, failures;
#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static void test_writer(void)
{
    char buf[256];
    json_writer_t w;

    jw_init(&w, buf, sizeof(buf));
    jw_obj_begin(&w);
    jw_kv_str(&w, "a", "x\"y\\z\n\t\x01");
    jw_kv_int(&w, "b", -42);
    jw_kv_uint(&w, "c", 4294967295u);
    jw_kv_bool(&w, "d", true);
    jw_key(&w, "e");
    jw_null(&w);
    jw_key(&w, "f");
    jw_arr_begin(&w);
    jw_int(&w, 1);
    jw_str(&w, NULL);
    jw_obj_begin(&w);
    jw_obj_end(&w);
    jw_arr_end(&w);
    jw_kv_fixed(&w, "g", -125, 2);
    jw_kv_fixed(&w, "h", 5, 2);
    jw_kv_fixed(&w, "i", -5, 1);
    jw_kv_fixed(&w, "j", 7, 0);
    jw_kv_fixed(&w, "k", LLONG_MIN, 0);
    jw_obj_end(&w);
    CHECK(jw_ok(&w));
    CHECK(strcmp(buf, "{\"a\":\"x\\\"y\\\\z\\n\\t\\u0001\",\"b\":-42,\"c\":4294967295,\"d\":true,\"e\":null,"
                      "\"f\":[1,null,{}],\"g\":-1.25,\"h\":0.05,\"i\":-0.5,\"j\":7,"
                      "\"k\":-9223372036854775808}") == 0);

    /* every truncation length must report overflow and stay NUL terminated */
    /* {"key":"a fairly long value"} is 29 characters: it needs cap >= 30 */
    for (size_t cap = 1; cap < 30; cap++) {
        char small[40];
        memset(small, 'Z', sizeof(small));
        jw_init(&w, small, cap);
        jw_obj_begin(&w);
        jw_kv_str(&w, "key", "a fairly long value");
        jw_obj_end(&w);
        CHECK(!jw_ok(&w));
        CHECK(strlen(small) < cap);
        CHECK(small[cap] == 'Z' || cap == sizeof(small));
    }

    /* unbalanced documents are not ok */
    jw_init(&w, buf, sizeof(buf));
    jw_obj_begin(&w);
    CHECK(!jw_ok(&w));
    jw_key(&w, "x");
    jw_obj_end(&w); /* key without value */
    CHECK(!jw_ok(&w));

    /* nesting deeper than the limit is refused */
    jw_init(&w, buf, sizeof(buf));
    for (int i = 0; i < JSON_WRITER_MAX_DEPTH + 1; i++) {
        jw_arr_begin(&w);
    }
    CHECK(w.overflow);
}

static json_field_result_t get(const char *json, const char *key, char *out, size_t out_len)
{
    return json_flat_get_string(json, strlen(json), key, out, out_len);
}

static void test_reader(void)
{
    char out[16];
    bool b = false;

    CHECK(json_flat_valid("{}", 2));
    const char spaced[] = " { \"a\" : 1 , \"b\":\"x\" } \n";
    CHECK(json_flat_valid(spaced, sizeof(spaced) - 1));
    CHECK(!json_flat_valid(spaced, sizeof(spaced))); /* trailing NUL is garbage */
    CHECK(!json_flat_valid("", 0));
    CHECK(!json_flat_valid("[]", 2));
    CHECK(!json_flat_valid("{\"a\":{}}", 8));
    CHECK(!json_flat_valid("{\"a\":[1]}", 9));
    CHECK(!json_flat_valid("{\"a\":1,}", 8));
    CHECK(!json_flat_valid("{\"a\" 1}", 7));
    CHECK(!json_flat_valid("{\"a\":1} x", 9));
    CHECK(!json_flat_valid("{\"a\":01}", 8));
    CHECK(!json_flat_valid("{\"a\":1.}", 8));
    CHECK(!json_flat_valid("{\"a\":-}", 7));
    CHECK(!json_flat_valid("{\"a\":1e}", 8));
    CHECK(json_flat_valid("{\"a\":-0.5e+3}", 13));
    CHECK(!json_flat_valid("{\"a\":tru}", 9));
    CHECK(!json_flat_valid("{\"a\":\"x}", 8));
    CHECK(!json_flat_valid("{\"a\":\"\x01\"}", 9));
    CHECK(!json_flat_valid("{\"a\":\"\\x\"}", 10));
    CHECK(!json_flat_valid("{\"a\":\"\\u12\"}", 12));
    CHECK(!json_flat_valid("{\"a\":\"\\ud800\"}", 14));
    CHECK(!json_flat_valid("{\"a\":1", 6));
    CHECK(!json_flat_valid(NULL, 0));

    CHECK(get("{\"password\":\"secret\"}", "password", out, sizeof(out)) == JSON_FIELD_OK && strcmp(out, "secret") == 0);
    CHECK(get("{\"x\":1}", "password", out, sizeof(out)) == JSON_FIELD_ABSENT);
    CHECK(get("{\"password\":1}", "password", out, sizeof(out)) == JSON_FIELD_INVALID);
    CHECK(get("{\"password\":\"x\",}", "password", out, sizeof(out)) == JSON_FIELD_INVALID);
    /* escapes */
    CHECK(get("{\"k\":\"a\\\"b\\\\c\\/d\\n\"}", "k", out, sizeof(out)) == JSON_FIELD_OK && strcmp(out, "a\"b\\c/d\n") == 0);
    CHECK(get("{\"k\":\"\\u0041\\u00e9\\u20ac\"}", "k", out, sizeof(out)) == JSON_FIELD_OK &&
          strcmp(out, "A\xc3\xa9\xe2\x82\xac") == 0);
    /* raw UTF-8 passes through */
    CHECK(get("{\"k\":\"\xc3\xa9t\xc3\xa9\"}", "k", out, sizeof(out)) == JSON_FIELD_OK && strcmp(out, "\xc3\xa9t\xc3\xa9") == 0);
    /* last duplicate wins, like JSON.parse */
    CHECK(get("{\"k\":\"a\",\"k\":\"b\"}", "k", out, sizeof(out)) == JSON_FIELD_OK && strcmp(out, "b") == 0);
    /* exactly fitting and one too long */
    CHECK(get("{\"k\":\"123456789012345\"}", "k", out, sizeof(out)) == JSON_FIELD_OK && strlen(out) == 15);
    CHECK(get("{\"k\":\"1234567890123456\"}", "k", out, sizeof(out)) == JSON_FIELD_INVALID && out[0] == '\0');
    /* key with embedded NUL does not match */
    CHECK(get("{\"k\\u0000\":\"x\"}", "k", out, sizeof(out)) == JSON_FIELD_ABSENT);
    /* very long keys are skipped, not truncated into a match */
    CHECK(get("{\"kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk\":\"x\"}",
              "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk", out, sizeof(out)) ==
          JSON_FIELD_ABSENT);
    /* length bounded: the parser must not read past len */
    const char longer[] = "{\"k\":\"v\"}GARBAGE";
    CHECK(json_flat_get_string(longer, 9, "k", out, sizeof(out)) == JSON_FIELD_OK && strcmp(out, "v") == 0);
    CHECK(json_flat_get_string(longer, 8, "k", out, sizeof(out)) == JSON_FIELD_INVALID);

    CHECK(json_flat_get_bool("{\"b\":true}", 10, "b", &b) == JSON_FIELD_OK && b);
    CHECK(json_flat_get_bool("{\"b\":false}", 11, "b", &b) == JSON_FIELD_OK && !b);
    CHECK(json_flat_get_bool("{\"b\":\"true\"}", 12, "b", &b) == JSON_FIELD_INVALID);
    CHECK(json_flat_get_bool("{}", 2, "b", &b) == JSON_FIELD_ABSENT);

    /* every prefix of a valid document is invalid (no out-of-bounds reads under ASan) */
    const char doc[] = "{\"password\":\"p\\u00e9ss\",\"n\":-1.5e3,\"t\":true,\"z\":null}";
    for (size_t i = 0; i < sizeof(doc) - 1; i++) {
        char copy[sizeof(doc)];
        memcpy(copy, doc, i);
        CHECK(!json_flat_valid(copy, i));
    }
    CHECK(json_flat_valid(doc, sizeof(doc) - 1));
}

int main(void)
{
    test_writer();
    test_reader();
    printf("json_util: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
