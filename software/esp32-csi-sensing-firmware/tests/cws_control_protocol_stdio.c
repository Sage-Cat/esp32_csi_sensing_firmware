/* SPDX-FileCopyrightText: 2026 Sage-Cat
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only stdio adapter used by the workspace cross-mean contract check.
 */

#include "cws_control_protocol.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t ping_hz;
} fixture_t;

static int apply_ping(void *opaque, uint32_t ping_hz)
{
    fixture_t *fixture = opaque;
    fixture->ping_hz = ping_hz;
    return 0;
}

static size_t describe_state(void *opaque, char *out, size_t out_size)
{
    (void)opaque;
    return (size_t)snprintf(
        out, out_size,
        "firmware_profile=cross-mean-test node_id=source-a rate_min_hz=0 "
        "rate_max_hz=50 band=2g channel=6 bssid=001122334455");
}

int main(void)
{
    fixture_t fixture = {.ping_hz = 10};
    cws_control_config_t config = {
        .boot_epoch = 7,
        .config_epoch = 4,
        .ping_hz = 10,
        .apply_ping = apply_ping,
        .describe_state = describe_state,
        .callback_context = &fixture,
    };
    cws_control_context_t control;
    char line[CWS_CONTROL_MAX_LINE + 2];
    char reply[CWS_CONTROL_MAX_REPLY];

    cws_control_init(&control, &config);
    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (cws_control_handle(&control, line, strlen(line), reply,
                               sizeof(reply)) < 0) {
            return 2;
        }
        if (fputs(reply, stdout) == EOF || fflush(stdout) != 0) {
            return 3;
        }
    }
    return ferror(stdin) ? 4 : 0;
}
