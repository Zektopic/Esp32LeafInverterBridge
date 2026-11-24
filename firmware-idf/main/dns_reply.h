/*
 * dns_reply.h - DNS responder logic of the captive portal.
 *
 * Every A query is answered with the bridge's own address, so a phone that
 * joins the access point finds the configuration page through its operating
 * system's captive-portal check ("Sign in to network"). AAAA and other types
 * get an empty NOERROR answer, non-query opcodes NOTIMP, malformed packets
 * are dropped.
 *
 * Plain C99 without ESP-IDF dependencies; unit tested on the host
 * (test/host/test_dns_reply.c).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DNS_REPLY_TTL_S 30u

/*
 * Build the reply to `query` (`query_len` bytes) into `out`. `ipv4` is in
 * network byte order as four bytes a.b.c.d -> {a, b, c, d}.
 * Returns the reply length, or 0 if the packet must be ignored.
 */
size_t dns_build_reply(const uint8_t *query, size_t query_len, const uint8_t ipv4[4], uint8_t *out,
                       size_t out_cap);

#ifdef __cplusplus
}
#endif
