/* SPDX-FileCopyrightText: 2026 Sage-Cat
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded, transport-agnostic control state machine for the CWS firmware
 * endpoint.  It deliberately has no ESP-IDF headers so its semantics can be
 * exercised by a normal host C compiler.
 */

#ifndef CWS_CONTROL_PROTOCOL_H
#define CWS_CONTROL_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define CWS_CONTROL_PROTOCOL "cws-firmware-control/1"
#define CWS_CONTROL_MAX_LINE 256
#define CWS_CONTROL_MAX_ID 48
#define CWS_CONTROL_MAX_REPLY 1024
#define CWS_CONTROL_REPLAY_SLOTS 8
#define CWS_CONTROL_PREPARED_SLOTS 4

typedef int (*cws_control_apply_ping_fn)(void *context, uint32_t ping_hz);
typedef size_t (*cws_control_describe_state_fn)(void *context, char *out,
                                                 size_t out_size);

typedef struct {
    uint32_t boot_epoch;
    uint32_t config_epoch;
    uint32_t ping_hz;
    cws_control_apply_ping_fn apply_ping;
    cws_control_describe_state_fn describe_state;
    void *callback_context;
} cws_control_config_t;

typedef struct {
    int used;
    char command_id[CWS_CONTROL_MAX_ID + 1];
    char transaction_id[CWS_CONTROL_MAX_ID + 1];
    char canonical[CWS_CONTROL_MAX_LINE + 1];
    char reply[CWS_CONTROL_MAX_REPLY];
} cws_control_replay_entry_t;

typedef struct {
    int used;
    char command_id[CWS_CONTROL_MAX_ID + 1];
    char transaction_id[CWS_CONTROL_MAX_ID + 1];
    uint32_t expected_config_epoch;
    uint32_t desired_ping_hz;
} cws_control_prepared_entry_t;

typedef struct {
    uint32_t boot_epoch;
    uint32_t config_epoch;
    uint32_t ping_hz;
    cws_control_apply_ping_fn apply_ping;
    cws_control_describe_state_fn describe_state;
    void *callback_context;
    unsigned int replay_next;
    unsigned int prepared_next;
    cws_control_replay_entry_t replay[CWS_CONTROL_REPLAY_SLOTS];
    cws_control_prepared_entry_t prepared[CWS_CONTROL_PREPARED_SLOTS];
} cws_control_context_t;

void cws_control_init(cws_control_context_t *context,
                      const cws_control_config_t *config);

/*
 * Handle exactly one framed ASCII line.  The reply is always NUL-terminated
 * when reply_size is nonzero.  A negative return only means the supplied reply
 * buffer was unusably small; protocol rejection is encoded in the reply.
 */
int cws_control_handle(cws_control_context_t *context, const char *line,
                       size_t line_size, char *reply, size_t reply_size);

uint32_t cws_control_config_epoch(const cws_control_context_t *context);
uint32_t cws_control_ping_hz(const cws_control_context_t *context);

/* Keep legacy control paths observable to the versioned epoch/state view. */
void cws_control_sync_external_ping(cws_control_context_t *context,
                                    uint32_t ping_hz);

#endif
