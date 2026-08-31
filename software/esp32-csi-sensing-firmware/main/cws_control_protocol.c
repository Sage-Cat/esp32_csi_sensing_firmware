/* SPDX-FileCopyrightText: 2026 Sage-Cat
 * SPDX-License-Identifier: Apache-2.0 */

#include "cws_control_protocol.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    CWS_OP_INVALID = 0,
    CWS_OP_CAPABILITIES,
    CWS_OP_GET_STATE,
    CWS_OP_PREPARE,
    CWS_OP_APPLY,
    CWS_OP_QUERY,
    CWS_OP_RESTORE,
} cws_operation_t;

typedef struct {
    cws_operation_t operation;
    const char *operation_name;
    char command_id[CWS_CONTROL_MAX_ID + 1];
    char transaction_id[CWS_CONTROL_MAX_ID + 1];
    char prepared_command_id[CWS_CONTROL_MAX_ID + 1];
    char query_command_id[CWS_CONTROL_MAX_ID + 1];
    uint32_t expected_config_epoch;
    uint32_t ping_hz;
    uint32_t prior_ping_hz;
    int has_expected_config_epoch;
    int has_ping_hz;
    int has_prior_ping_hz;
    int has_prepared_command_id;
    int has_query_command_id;
} cws_request_t;

static int append(char *out, size_t out_size, size_t *used, const char *fmt,
                  ...)
{
    va_list args;
    int written;
    if (*used >= out_size) {
        return -1;
    }
    va_start(args, fmt);
    written = vsnprintf(out + *used, out_size - *used, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= out_size - *used) {
        out[out_size - 1] = '\0';
        return -1;
    }
    *used += (size_t)written;
    return 0;
}

static int id_is_valid(const char *value)
{
    size_t length = strlen(value);
    if (length == 0 || length > CWS_CONTROL_MAX_ID) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned char current = (unsigned char)value[index];
        if (!(isalnum(current) || current == '.' || current == '_' ||
              current == '-')) {
            return 0;
        }
    }
    return 1;
}

static int parse_u32(const char *value, uint32_t *result)
{
    uint64_t parsed = 0;
    if (*value == '\0') {
        return 0;
    }
    for (; *value != '\0'; value++) {
        if (*value < '0' || *value > '9') {
            return 0;
        }
        parsed = parsed * 10U + (uint32_t)(*value - '0');
        if (parsed > UINT32_MAX) {
            return 0;
        }
    }
    *result = (uint32_t)parsed;
    return 1;
}

static cws_operation_t parse_operation(const char *value)
{
    if (strcmp(value, "CAPABILITIES") == 0) return CWS_OP_CAPABILITIES;
    if (strcmp(value, "GET_STATE") == 0) return CWS_OP_GET_STATE;
    if (strcmp(value, "PREPARE") == 0) return CWS_OP_PREPARE;
    if (strcmp(value, "APPLY") == 0) return CWS_OP_APPLY;
    if (strcmp(value, "QUERY") == 0) return CWS_OP_QUERY;
    if (strcmp(value, "RESTORE") == 0) return CWS_OP_RESTORE;
    return CWS_OP_INVALID;
}

static const char *operation_name(cws_operation_t operation)
{
    switch (operation) {
    case CWS_OP_CAPABILITIES: return "capabilities";
    case CWS_OP_GET_STATE: return "get-state";
    case CWS_OP_PREPARE: return "prepare";
    case CWS_OP_APPLY: return "apply";
    case CWS_OP_QUERY: return "query";
    case CWS_OP_RESTORE: return "restore";
    default: return "unknown";
    }
}

/* 0 malformed, 1 valid, 2 unknown protocol, 3 unknown operation, 4 inapplicable field. */
static int parse_request(const char *line, size_t line_size,
                         cws_request_t *request, char *canonical,
                         size_t canonical_size)
{
    char local[CWS_CONTROL_MAX_LINE + 1];
    char *token;
    unsigned int seen = 0;
    enum { SEEN_OPERATION = 1, SEEN_COMMAND = 2, SEEN_TRANSACTION = 4,
           SEEN_EXPECTED = 8, SEEN_PING = 16, SEEN_PRIOR = 32,
           SEEN_PREPARED = 64, SEEN_QUERY = 128 };

    if (line_size == 0 || line_size > CWS_CONTROL_MAX_LINE) return 0;
    for (size_t index = 0; index < line_size; index++) {
        unsigned char current = (unsigned char)line[index];
        if (current == '\r' || current == '\n') continue;
        if (current < 0x20 || current > 0x7e) return 0;
    }
    memcpy(local, line, line_size);
    local[line_size] = '\0';
    while (line_size > 0 && (local[line_size - 1] == '\n' ||
                             local[line_size - 1] == '\r')) {
        local[--line_size] = '\0';
    }
    if (line_size == 0 || local[0] == ' ' || local[line_size - 1] == ' ') return 0;
    for (size_t index = 1; index < line_size; index++)
        if (local[index] == ' ' && local[index - 1] == ' ') return 0;
    memset(request, 0, sizeof(*request));
    token = strtok(local, " ");
    if (token == NULL) return 0;
    if (strcmp(token, CWS_CONTROL_PROTOCOL) != 0) return 2;
    while ((token = strtok(NULL, " ")) != NULL) {
        char *equals = strchr(token, '=');
        char *key;
        char *value;
        if (equals == NULL || equals == token || strchr(equals + 1, '=') != NULL)
            return 0;
        *equals = '\0';
        key = token;
        value = equals + 1;
        if (strcmp(key, "operation") == 0) {
            if (seen & SEEN_OPERATION) return 0;
            request->operation = parse_operation(value);
            if (request->operation == CWS_OP_INVALID) return 3;
            request->operation_name = operation_name(request->operation);
            seen |= SEEN_OPERATION;
        } else if (strcmp(key, "command_id") == 0) {
            if ((seen & SEEN_COMMAND) || !id_is_valid(value)) return 0;
            strcpy(request->command_id, value); seen |= SEEN_COMMAND;
        } else if (strcmp(key, "transaction_id") == 0) {
            if ((seen & SEEN_TRANSACTION) || !id_is_valid(value)) return 0;
            strcpy(request->transaction_id, value); seen |= SEEN_TRANSACTION;
        } else if (strcmp(key, "expected_config_epoch") == 0) {
            if ((seen & SEEN_EXPECTED) || !parse_u32(value, &request->expected_config_epoch)) return 0;
            request->has_expected_config_epoch = 1; seen |= SEEN_EXPECTED;
        } else if (strcmp(key, "ping_hz") == 0) {
            if ((seen & SEEN_PING) || !parse_u32(value, &request->ping_hz)) return 0;
            request->has_ping_hz = 1; seen |= SEEN_PING;
        } else if (strcmp(key, "prior_ping_hz") == 0) {
            if ((seen & SEEN_PRIOR) || !parse_u32(value, &request->prior_ping_hz)) return 0;
            request->has_prior_ping_hz = 1; seen |= SEEN_PRIOR;
        } else if (strcmp(key, "prepared_command_id") == 0) {
            if ((seen & SEEN_PREPARED) || !id_is_valid(value)) return 0;
            strcpy(request->prepared_command_id, value);
            request->has_prepared_command_id = 1; seen |= SEEN_PREPARED;
        } else if (strcmp(key, "query_command_id") == 0) {
            if ((seen & SEEN_QUERY) || !id_is_valid(value)) return 0;
            strcpy(request->query_command_id, value);
            request->has_query_command_id = 1; seen |= SEEN_QUERY;
        } else {
            return 0;
        }
    }
    if ((seen & (SEEN_OPERATION | SEEN_COMMAND | SEEN_TRANSACTION)) !=
        (SEEN_OPERATION | SEEN_COMMAND | SEEN_TRANSACTION)) return 0;
    switch (request->operation) {
    case CWS_OP_CAPABILITIES:
    case CWS_OP_GET_STATE:
        if (seen & ~(SEEN_OPERATION | SEEN_COMMAND | SEEN_TRANSACTION)) return 4;
        break;
    case CWS_OP_PREPARE:
        if (seen & ~(SEEN_OPERATION | SEEN_COMMAND | SEEN_TRANSACTION |
                     SEEN_EXPECTED | SEEN_PING)) return 4;
        break;
    case CWS_OP_APPLY:
        if (seen & ~(SEEN_OPERATION | SEEN_COMMAND | SEEN_TRANSACTION |
                     SEEN_PREPARED)) return 4;
        break;
    case CWS_OP_QUERY:
        if (seen & ~(SEEN_OPERATION | SEEN_COMMAND | SEEN_TRANSACTION |
                     SEEN_QUERY)) return 4;
        break;
    case CWS_OP_RESTORE:
        if (seen & ~(SEEN_OPERATION | SEEN_COMMAND | SEEN_TRANSACTION |
                     SEEN_EXPECTED | SEEN_PRIOR)) return 4;
        break;
    default:
        return 0;
    }
    if ((request->operation == CWS_OP_PREPARE &&
         (!request->has_expected_config_epoch || !request->has_ping_hz)) ||
        (request->operation == CWS_OP_APPLY && !request->has_prepared_command_id) ||
        (request->operation == CWS_OP_QUERY && !request->has_query_command_id) ||
        (request->operation == CWS_OP_RESTORE &&
         (!request->has_expected_config_epoch || !request->has_prior_ping_hz))) return 0;
    if ((request->has_ping_hz && request->ping_hz > 50) ||
        (request->has_prior_ping_hz && request->prior_ping_hz > 50)) return 0;
    int written = snprintf(canonical, canonical_size,
                    "op=%s;cmd=%s;tx=%s;expected=%s%" PRIu32 ";ping=%s%" PRIu32
                    ";prior=%s%" PRIu32 ";prepared=%s%s;query=%s%s",
                    request->operation_name, request->command_id,
                    request->transaction_id,
                    request->has_expected_config_epoch ? "" : "-", request->expected_config_epoch,
                    request->has_ping_hz ? "" : "-", request->ping_hz,
                    request->has_prior_ping_hz ? "" : "-", request->prior_ping_hz,
                    request->has_prepared_command_id ? "" : "-", request->prepared_command_id,
                    request->has_query_command_id ? "" : "-", request->query_command_id);
    return written > 0 && (size_t)written < canonical_size;
}

static int reply_status(cws_control_context_t *context, const cws_request_t *request,
                        const char *status, const char *reason, char *reply,
                        size_t reply_size, int include_description,
                        const char *requested_ping_hz)
{
    size_t used = 0;
    char description[480] = {0};
    if (include_description && context->describe_state != NULL) {
        context->describe_state(context->callback_context, description,
                                sizeof(description));
    }
    return append(reply, reply_size, &used,
                  CWS_CONTROL_PROTOCOL " operation=%s command_id=%s transaction_id=%s"
                  " status=%s reason=%s boot_epoch=%" PRIu32 " config_epoch=%" PRIu32
                  "%s%s effective_ping_hz=%" PRIu32 " active=%u%s%s\n",
                  request ? request->operation_name : "unknown",
                  request ? request->command_id : "invalid",
                  request ? request->transaction_id : "invalid", status, reason,
                  context->boot_epoch, context->config_epoch,
                  requested_ping_hz ? " requested_ping_hz=" : "",
                  requested_ping_hz ? requested_ping_hz : "",
                  context->ping_hz,
                  context->ping_hz ? 1U : 0U,
                  description[0] ? " " : "", description);
}

static cws_control_replay_entry_t *find_replay(cws_control_context_t *context,
                                                const char *command_id)
{
    for (unsigned int index = 0; index < CWS_CONTROL_REPLAY_SLOTS; index++)
        if (context->replay[index].used &&
            strcmp(context->replay[index].command_id, command_id) == 0)
            return &context->replay[index];
    return NULL;
}

static void cache_reply(cws_control_context_t *context, const cws_request_t *request,
                        const char *canonical, const char *reply)
{
    cws_control_replay_entry_t *entry =
        &context->replay[context->replay_next++ % CWS_CONTROL_REPLAY_SLOTS];
    entry->used = 1;
    strcpy(entry->command_id, request->command_id);
    strcpy(entry->transaction_id, request->transaction_id);
    strcpy(entry->canonical, canonical);
    strncpy(entry->reply, reply, sizeof(entry->reply) - 1);
    entry->reply[sizeof(entry->reply) - 1] = '\0';
}

static cws_control_prepared_entry_t *find_prepared(cws_control_context_t *context,
                                                    const char *command_id)
{
    for (unsigned int index = 0; index < CWS_CONTROL_PREPARED_SLOTS; index++)
        if (context->prepared[index].used &&
            strcmp(context->prepared[index].command_id, command_id) == 0)
            return &context->prepared[index];
    return NULL;
}

void cws_control_init(cws_control_context_t *context,
                      const cws_control_config_t *config)
{
    memset(context, 0, sizeof(*context));
    context->boot_epoch = config->boot_epoch;
    context->config_epoch = config->config_epoch;
    context->ping_hz = config->ping_hz;
    context->apply_ping = config->apply_ping;
    context->describe_state = config->describe_state;
    context->callback_context = config->callback_context;
}

int cws_control_handle(cws_control_context_t *context, const char *line,
                       size_t line_size, char *reply, size_t reply_size)
{
    cws_request_t request;
    char canonical[CWS_CONTROL_MAX_LINE + 1];
    cws_control_replay_entry_t *replay;
    const char *status = "ok";
    const char *reason = "ok";
    int include_description = 0;
    if (reply_size == 0) return -1;
    reply[0] = '\0';
    int parse_result = parse_request(line, line_size, &request, canonical,
                                     sizeof(canonical));
    if (parse_result != 1) {
        const char *reason = parse_result == 2 ? "unknown-protocol" :
                             parse_result == 3 ? "unknown-operation" :
                             parse_result == 4 ? "inapplicable-field" :
                             "malformed-request";
        return reply_status(context, NULL, "rejected", reason, reply,
                            reply_size, 0, NULL);
    }
    replay = find_replay(context, request.command_id);
    if (replay != NULL) {
        if (strcmp(replay->canonical, canonical) == 0) {
            strncpy(reply, replay->reply, reply_size - 1);
            reply[reply_size - 1] = '\0';
            return 0;
        }
        return reply_status(context, &request, "rejected", "command-id-conflict",
                            reply, reply_size, 0, NULL);
    }
    switch (request.operation) {
    case CWS_OP_CAPABILITIES:
        include_description = 1;
        break;
    case CWS_OP_GET_STATE:
        include_description = 1;
        break;
    case CWS_OP_QUERY: {
        cws_control_replay_entry_t *queried =
            find_replay(context, request.query_command_id);
        if (queried == NULL) {
            status = "rejected"; reason = "unknown-command";
            break;
        }
        if (strcmp(queried->transaction_id, request.transaction_id) != 0) {
            status = "rejected"; reason = "transaction-mismatch";
            break;
        }
        strncpy(reply, queried->reply, reply_size - 1);
        reply[reply_size - 1] = '\0';
        cache_reply(context, &request, canonical, reply);
        return 0;
    }
    case CWS_OP_PREPARE:
        if (request.expected_config_epoch != context->config_epoch) {
            status = "rejected"; reason = "stale-config-epoch";
        } else {
            cws_control_prepared_entry_t *prepared =
                &context->prepared[context->prepared_next++ % CWS_CONTROL_PREPARED_SLOTS];
            prepared->used = 1;
            strcpy(prepared->command_id, request.command_id);
            strcpy(prepared->transaction_id, request.transaction_id);
            prepared->expected_config_epoch = request.expected_config_epoch;
            prepared->desired_ping_hz = request.ping_hz;
            status = "prepared"; reason = "prepared";
        }
        break;
    case CWS_OP_APPLY: {
        cws_control_prepared_entry_t *prepared =
            find_prepared(context, request.prepared_command_id);
        if (prepared == NULL) {
            status = "rejected"; reason = "unknown-prepare";
        } else {
            request.ping_hz = prepared->desired_ping_hz;
            request.has_ping_hz = 1;
        }
        if (prepared != NULL && strcmp(prepared->transaction_id, request.transaction_id) != 0) {
            status = "rejected"; reason = "transaction-mismatch";
        } else if (prepared != NULL && prepared->expected_config_epoch != context->config_epoch) {
            status = "rejected"; reason = "stale-config-epoch";
        } else if (prepared != NULL && prepared->desired_ping_hz == context->ping_hz) {
            status = "applied"; reason = "already-effective";
            prepared->used = 0;
        } else if (prepared != NULL && context->apply_ping == NULL) {
            status = "rejected"; reason = "apply-failed";
        } else if (prepared != NULL) {
            int callback_result = context->apply_ping(context->callback_context,
                                                      prepared->desired_ping_hz);
            if (callback_result != 0) {
                status = "rejected";
                reason = callback_result == -2 ? "apply-state-uncertain" : "apply-failed";
            } else {
                context->ping_hz = prepared->desired_ping_hz;
                context->config_epoch++;
                prepared->used = 0;
                status = "applied"; reason = "applied";
            }
        }
        break;
    }
    case CWS_OP_RESTORE:
        if (request.expected_config_epoch != context->config_epoch) {
            status = "rejected"; reason = "stale-config-epoch";
        } else if (request.prior_ping_hz == context->ping_hz) {
            status = "restored"; reason = "already-effective";
        } else if (context->apply_ping == NULL) {
            status = "rejected"; reason = "restore-failed";
        } else {
            int callback_result = context->apply_ping(context->callback_context,
                                                      request.prior_ping_hz);
            if (callback_result != 0) {
                status = "rejected";
                reason = callback_result == -2 ? "restore-state-uncertain" : "restore-failed";
            } else {
                context->ping_hz = request.prior_ping_hz;
                context->config_epoch++;
                status = "restored"; reason = "restored";
            }
        }
        break;
    default:
        status = "rejected"; reason = "unknown-operation";
        break;
    }
    char requested_ping[16];
    const char *requested_ping_hz = NULL;
    if (request.operation == CWS_OP_PREPARE) {
        snprintf(requested_ping, sizeof(requested_ping), "%" PRIu32, request.ping_hz);
        requested_ping_hz = requested_ping;
    } else if (request.operation == CWS_OP_RESTORE) {
        snprintf(requested_ping, sizeof(requested_ping), "%" PRIu32, request.prior_ping_hz);
        requested_ping_hz = requested_ping;
    } else if (request.operation == CWS_OP_APPLY) {
        if (request.has_ping_hz) {
            snprintf(requested_ping, sizeof(requested_ping), "%" PRIu32,
                     request.ping_hz);
            requested_ping_hz = requested_ping;
        } else {
            requested_ping_hz = "unknown";
        }
    }
    if (reply_status(context, &request, status, reason, reply, reply_size,
                     include_description, requested_ping_hz) != 0) return -1;
    cache_reply(context, &request, canonical, reply);
    return 0;
}

uint32_t cws_control_config_epoch(const cws_control_context_t *context)
{
    return context->config_epoch;
}

uint32_t cws_control_ping_hz(const cws_control_context_t *context)
{
    return context->ping_hz;
}

void cws_control_sync_external_ping(cws_control_context_t *context,
                                    uint32_t ping_hz)
{
    if (ping_hz <= 50 && context->ping_hz != ping_hz) {
        context->ping_hz = ping_hz;
        context->config_epoch++;
    }
}
