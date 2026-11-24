/*
 * dns_reply.c - DNS responder logic of the captive portal.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "dns_reply.h"

#include <string.h>

#define HDR_LEN        12u
#define FLAG_QR        0x8000u
#define FLAG_AA        0x0400u
#define FLAG_TC        0x0200u
#define FLAG_RD        0x0100u
#define OPCODE_MASK    0x7800u
#define RCODE_NOTIMP   4u
#define TYPE_A         1u
#define TYPE_ANY       255u
#define CLASS_IN       1u
#define MAX_NAME_LEN   255u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* Length of the uncompressed QNAME at q[off], or 0 if malformed. */
static size_t qname_len(const uint8_t *q, size_t len, size_t off)
{
    size_t pos = off;
    size_t name_len = 0;
    for (;;) {
        if (pos >= len) {
            return 0;
        }
        const uint8_t label = q[pos];
        if (label == 0) {
            return pos + 1 - off;
        }
        if (label & 0xC0) {
            return 0; /* compression pointers do not belong in a question */
        }
        name_len += (size_t)label + 1u;
        if (name_len > MAX_NAME_LEN) {
            return 0;
        }
        pos += (size_t)label + 1u;
    }
}

size_t dns_build_reply(const uint8_t *query, size_t query_len, const uint8_t ipv4[4], uint8_t *out,
                       size_t out_cap)
{
    if (query == NULL || out == NULL || query_len < HDR_LEN || out_cap < HDR_LEN) {
        return 0;
    }
    const uint16_t flags = rd16(&query[2]);
    if (flags & FLAG_QR) {
        return 0; /* a response, not a query */
    }
    const uint16_t qdcount = rd16(&query[4]);
    const uint16_t rd = (uint16_t)(flags & FLAG_RD);

    /* Header: same id, response, authoritative. */
    memcpy(out, query, 2);
    if ((flags & OPCODE_MASK) != 0 || qdcount != 1) {
        wr16(&out[2], (uint16_t)(FLAG_QR | (flags & OPCODE_MASK) | rd | RCODE_NOTIMP));
        memset(&out[4], 0, 8);
        return HDR_LEN;
    }

    const size_t nlen = qname_len(query, query_len, HDR_LEN);
    if (nlen == 0 || HDR_LEN + nlen + 4u > query_len) {
        return 0;
    }
    const size_t qend = HDR_LEN + nlen + 4u;
    const uint16_t qtype = rd16(&query[HDR_LEN + nlen]);
    const uint16_t qclass = rd16(&query[HDR_LEN + nlen + 2u]);
    const int answer = (qclass == CLASS_IN && (qtype == TYPE_A || qtype == TYPE_ANY));
    const size_t reply_len = qend + (answer ? 16u : 0u);
    if (reply_len > out_cap) {
        return 0;
    }

    wr16(&out[2], (uint16_t)(FLAG_QR | FLAG_AA | rd));
    wr16(&out[4], 1);                     /* QDCOUNT */
    wr16(&out[6], answer ? 1u : 0u);      /* ANCOUNT */
    wr16(&out[8], 0);                     /* NSCOUNT */
    wr16(&out[10], 0);                    /* ARCOUNT: EDNS OPT records are dropped */
    memcpy(&out[HDR_LEN], &query[HDR_LEN], nlen + 4u);

    if (answer) {
        uint8_t *a = &out[qend];
        wr16(&a[0], 0xC000u | HDR_LEN);  /* name: pointer to the question */
        wr16(&a[2], TYPE_A);
        wr16(&a[4], CLASS_IN);
        wr16(&a[6], 0);
        wr16(&a[8], (uint16_t)DNS_REPLY_TTL_S);
        wr16(&a[10], 4);                 /* RDLENGTH */
        memcpy(&a[12], ipv4, 4);
    }
    return reply_len;
}
