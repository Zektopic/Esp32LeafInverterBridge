/*
 * Host unit tests of main/dns_reply.c (run with ASan/UBSan, see CMakeLists.txt).
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dns_reply.h"

static int checks, failures;
#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static const uint8_t k_ip[4] = {192, 168, 4, 1};

/* Builds a standard query for `name` (dotted) with the given type. */
static size_t make_query(uint8_t *q, const char *name, uint16_t type, uint16_t flags)
{
    size_t n = 0;
    q[n++] = 0xAB;
    q[n++] = 0xCD;
    q[n++] = (uint8_t)(flags >> 8);
    q[n++] = (uint8_t)flags;
    q[n++] = 0;
    q[n++] = 1; /* QDCOUNT */
    memset(&q[n], 0, 6);
    n += 6;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        const size_t len = dot ? (size_t)(dot - p) : strlen(p);
        q[n++] = (uint8_t)len;
        memcpy(&q[n], p, len);
        n += len;
        p += len + (dot ? 1 : 0);
    }
    q[n++] = 0;
    q[n++] = (uint8_t)(type >> 8);
    q[n++] = (uint8_t)type;
    q[n++] = 0;
    q[n++] = 1; /* IN */
    return n;
}

int main(void)
{
    uint8_t q[512], r[512];

    /* A query for a captive-portal probe host */
    size_t qn = make_query(q, "connectivitycheck.gstatic.com", 1, 0x0100);
    size_t rn = dns_build_reply(q, qn, k_ip, r, sizeof(r));
    CHECK(rn == qn + 16);
    CHECK(r[0] == 0xAB && r[1] == 0xCD);
    CHECK(r[2] == 0x85 && r[3] == 0x00); /* QR AA RD, NOERROR */
    CHECK(r[5] == 1 && r[7] == 1 && r[9] == 0 && r[11] == 0);
    CHECK(memcmp(&r[12], &q[12], qn - 12) == 0);
    CHECK(r[qn] == 0xC0 && r[qn + 1] == 0x0C);
    CHECK(r[qn + 3] == 1 && r[qn + 5] == 1);
    CHECK(r[qn + 9] == DNS_REPLY_TTL_S && r[qn + 11] == 4);
    CHECK(memcmp(&r[qn + 12], k_ip, 4) == 0);

    /* AAAA: empty answer so the client falls back to IPv4 */
    qn = make_query(q, "captive.apple.com", 28, 0x0100);
    rn = dns_build_reply(q, qn, k_ip, r, sizeof(r));
    CHECK(rn == qn && r[7] == 0 && (r[3] & 0x0F) == 0);

    /* ANY is answered like A */
    qn = make_query(q, "x", 255, 0);
    CHECK(dns_build_reply(q, qn, k_ip, r, sizeof(r)) == qn + 16);

    /* responses are ignored */
    qn = make_query(q, "x", 1, 0x8000);
    CHECK(dns_build_reply(q, qn, k_ip, r, sizeof(r)) == 0);

    /* other opcodes (e.g. UPDATE) -> NOTIMP header only */
    qn = make_query(q, "x", 1, 0x2800);
    rn = dns_build_reply(q, qn, k_ip, r, sizeof(r));
    CHECK(rn == 12 && (r[3] & 0x0F) == 4 && (r[2] & 0x80));

    /* qdcount != 1 -> NOTIMP */
    qn = make_query(q, "x", 1, 0);
    q[5] = 2;
    rn = dns_build_reply(q, qn, k_ip, r, sizeof(r));
    CHECK(rn == 12 && (r[3] & 0x0F) == 4);

    /* compression pointer in the question -> dropped */
    qn = make_query(q, "x", 1, 0);
    q[12] = 0xC0;
    CHECK(dns_build_reply(q, qn, k_ip, r, sizeof(r)) == 0);

    /* name longer than 255 bytes -> dropped */
    char longname[400];
    memset(longname, 0, sizeof(longname));
    for (int i = 0; i < 6; i++) {
        strcat(longname, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."); /* 59 + dot */
    }
    strcat(longname, "com");
    qn = make_query(q, longname, 1, 0);
    CHECK(dns_build_reply(q, qn, k_ip, r, sizeof(r)) == 0);

    /* output buffer too small -> dropped, never overflowed */
    qn = make_query(q, "example.com", 1, 0);
    CHECK(dns_build_reply(q, qn, k_ip, r, qn + 15) == 0);
    CHECK(dns_build_reply(q, qn, k_ip, r, qn + 16) == qn + 16);

    /* every truncation of a valid query is handled (no reads past the end under ASan) */
    qn = make_query(q, "www.example.com", 1, 0x0100);
    for (size_t len = 0; len < qn; len++) {
        uint8_t *copy = malloc(len ? len : 1);
        memcpy(copy, q, len);
        const size_t n = dns_build_reply(copy, len, k_ip, r, sizeof(r));
        CHECK(n == 0 || n == 12);
        free(copy);
    }

    /* random garbage never crashes */
    srand(1);
    for (int i = 0; i < 20000; i++) {
        const size_t len = (size_t)(rand() % 64);
        uint8_t *copy = malloc(len ? len : 1);
        for (size_t j = 0; j < len; j++) {
            copy[j] = (uint8_t)rand();
        }
        const size_t n = dns_build_reply(copy, len, k_ip, r, sizeof(r));
        CHECK(n <= sizeof(r));
        free(copy);
    }

    printf("dns_reply: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
