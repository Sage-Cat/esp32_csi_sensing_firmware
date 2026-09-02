/*
 * SPDX-FileCopyrightText: 2026 Sage-Cat
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CWS_PROBE_TIMING_H
#define CWS_PROBE_TIMING_H

#include <stdbool.h>
#include <stdint.h>

#define CWS_PROBE_MAX_FREQUENCY_HZ 50U

bool cws_probe_timing_for_frequency(uint32_t frequency_hz,
                                    uint32_t *interval_ms,
                                    uint32_t *timeout_ms);
uint32_t cws_probe_timeout_ms(uint32_t frequency_hz);

#endif
