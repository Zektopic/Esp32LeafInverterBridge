/*
 * sysinfo.h - device diagnostics for GET /api/system.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "json_util.h"

/* Writes the diagnostics object (chip, firmware, partitions, memory, tasks, ...). */
void sysinfo_write_json(json_writer_t *w);

/* True if a core dump from a previous crash is stored in flash. */
bool sysinfo_coredump_present(size_t *size);

/* Human readable reason of the last reset. */
const char *sysinfo_reset_reason(void);
