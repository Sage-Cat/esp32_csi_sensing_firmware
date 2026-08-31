/* SPDX-FileCopyrightText: 2026 Sage-Cat
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef CWS_LEGACY_COMMANDS_H
#define CWS_LEGACY_COMMANDS_H

#include <stdint.h>

typedef enum {
    CWS_LEGACY_INVALID = 0,
    CWS_LEGACY_SET_PING_HZ,
    CWS_LEGACY_REBOOT,
} cws_legacy_command_t;

cws_legacy_command_t cws_legacy_parse(const char *line, uint32_t *ping_hz);

#endif
