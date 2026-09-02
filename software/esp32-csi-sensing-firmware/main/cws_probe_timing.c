/*
 * SPDX-FileCopyrightText: 2026 Sage-Cat
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cws_probe_timing.h"

#include <stddef.h>

bool cws_probe_timing_for_frequency(uint32_t frequency_hz,
                                    uint32_t *interval_ms,
                                    uint32_t *timeout_ms)
{
    if (frequency_hz == 0 || frequency_hz > CWS_PROBE_MAX_FREQUENCY_HZ ||
        interval_ms == NULL || timeout_ms == NULL) {
        return false;
    }

    *interval_ms = 1000U / frequency_hz;
    /*
     * esp_ping's default one-second receive timeout serializes each lost echo
     * reply with the next probe.  Bound that wait to one nominal interval so a
     * missed reply cannot pause a 40 Hz measurement stream for one second.
     */
    *timeout_ms = *interval_ms;
    return true;
}

uint32_t cws_probe_timeout_ms(uint32_t frequency_hz)
{
    uint32_t interval_ms;
    uint32_t timeout_ms;
    if (!cws_probe_timing_for_frequency(frequency_hz, &interval_ms,
                                        &timeout_ms)) {
        return 0;
    }
    return timeout_ms;
}
