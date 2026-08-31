/* SPDX-FileCopyrightText: 2026 Sage-Cat
 * SPDX-License-Identifier: Apache-2.0 */
#include "cws_legacy_commands.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

cws_legacy_command_t cws_legacy_parse(const char *line, uint32_t *ping_hz)
{
    char extra;
    if (strcmp(line, "CWS_REBOOT\n") == 0 || strcmp(line, "CWS_REBOOT\r\n") == 0)
        return CWS_LEGACY_REBOOT;
    if (sscanf(line, "CWS_SET_PING_HZ %" SCNu32 " %c", ping_hz, &extra) == 1)
        return CWS_LEGACY_SET_PING_HZ;
    return CWS_LEGACY_INVALID;
}
