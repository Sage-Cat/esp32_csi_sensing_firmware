#include "cws_control_protocol.h"
#include "cws_legacy_commands.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { int calls; uint32_t last; int failure_code; } fixture_t;

static int apply(void *opaque, uint32_t ping_hz)
{
    fixture_t *fixture = opaque;
    fixture->calls++;
    fixture->last = ping_hz;
    return fixture->failure_code;
}

static size_t describe(void *opaque, char *out, size_t out_size)
{
    (void)opaque;
    return (size_t)snprintf(out, out_size,
                            "firmware_profile=test node_id=test-node rate_min_hz=0 rate_max_hz=50 band=2g channel=6 bssid=001122334455");
}

static void handle(cws_control_context_t *control, const char *line, char *reply)
{
    assert(cws_control_handle(control, line, strlen(line), reply,
                              CWS_CONTROL_MAX_REPLY) == 0);
}

int main(void)
{
    uint32_t legacy_rate = 0;
    assert(cws_legacy_parse("CWS_SET_PING_HZ 20\n", &legacy_rate) == CWS_LEGACY_SET_PING_HZ && legacy_rate == 20);
    assert(cws_legacy_parse("CWS_SET_PING_HZ 20 trailing\n", &legacy_rate) == CWS_LEGACY_INVALID);
    assert(cws_legacy_parse("CWS_REBOOT\r\n", &legacy_rate) == CWS_LEGACY_REBOOT);
    fixture_t fixture = {0};
    cws_control_config_t config = { .boot_epoch = 17, .config_epoch = 0,
        .ping_hz = 20, .apply_ping = apply, .describe_state = describe,
        .callback_context = &fixture };
    cws_control_context_t control;
    char reply[CWS_CONTROL_MAX_REPLY], duplicate[CWS_CONTROL_MAX_REPLY];
    cws_control_init(&control, &config);

    handle(&control, "cws-firmware-control/1 operation=CAPABILITIES command_id=c1 transaction_id=t1\n", reply);
    assert(strstr(reply, "status=ok") && strstr(reply, "rate_max_hz=50"));
    handle(&control, "cws-firmware-control/1 operation=CAPABILITIES command_id=c1x transaction_id=t1 ping_hz=20\n", reply);
    assert(strstr(reply, "reason=inapplicable-field"));
    handle(&control, "cws-firmware-control/1 operation=GET_STATE command_id=c2 transaction_id=t1\n", reply);
    assert(strstr(reply, "effective_ping_hz=20"));
    handle(&control, "wrong/1 operation=GET_STATE command_id=c3 transaction_id=t1\n", reply);
    assert(strstr(reply, "reason=unknown-protocol"));
    handle(&control, "cws-firmware-control/1 operation=NOPE command_id=c4 transaction_id=t1\n", reply);
    assert(strstr(reply, "reason=unknown-operation"));
    handle(&control, "cws-firmware-control/1 operation=PREPARE command_id=p1 transaction_id=tx-a expected_config_epoch=0 ping_hz=51\n", reply);
    assert(strstr(reply, "reason=malformed-request"));

    handle(&control, "cws-firmware-control/1 operation=PREPARE command_id=p1 transaction_id=tx-a expected_config_epoch=0 ping_hz=40\n", reply);
    assert(strstr(reply, "status=prepared") && strstr(reply, "requested_ping_hz=40"));
    handle(&control, "cws-firmware-control/1 operation=APPLY command_id=a1 transaction_id=tx-a prepared_command_id=p1\n", reply);
    assert(strstr(reply, "status=applied") && strstr(reply, "config_epoch=1") &&
           strstr(reply, "requested_ping_hz=40"));
    assert(fixture.calls == 1 && fixture.last == 40);
    strcpy(duplicate, reply);
    /* A lost APPLY reply is recoverable without a second callback invocation. */
    handle(&control, "cws-firmware-control/1 operation=QUERY command_id=q1 transaction_id=tx-a query_command_id=a1\n", reply);
    assert(strcmp(reply, duplicate) == 0 && fixture.calls == 1);
    handle(&control, "cws-firmware-control/1 operation=QUERY command_id=q2 transaction_id=tx-a query_command_id=missing\n", reply);
    assert(strstr(reply, "reason=unknown-command"));
    handle(&control, "cws-firmware-control/1 operation=QUERY command_id=q3 transaction_id=other query_command_id=a1\n", reply);
    assert(strstr(reply, "reason=transaction-mismatch"));
    handle(&control, "cws-firmware-control/1 operation=APPLY command_id=a1 transaction_id=tx-a prepared_command_id=p1\n", reply);
    assert(strcmp(reply, duplicate) == 0 && fixture.calls == 1);
    handle(&control, "cws-firmware-control/1 operation=GET_STATE command_id=a1 transaction_id=other\n", reply);
    assert(strstr(reply, "reason=command-id-conflict"));
    handle(&control, "cws-firmware-control/1 operation=PREPARE command_id=p2 transaction_id=tx-b expected_config_epoch=0 ping_hz=10\n", reply);
    assert(strstr(reply, "reason=stale-config-epoch"));
    handle(&control, "cws-firmware-control/1 operation=PREPARE command_id=p3 transaction_id=tx-c expected_config_epoch=1 ping_hz=10\n", reply);
    handle(&control, "cws-firmware-control/1 operation=APPLY command_id=ax transaction_id=tx-c prepared_command_id=p3 ping_hz=10\n", reply);
    assert(strstr(reply, "reason=inapplicable-field"));
    handle(&control, "cws-firmware-control/1 operation=APPLY command_id=a3 transaction_id=wrong prepared_command_id=p3\n", reply);
    assert(strstr(reply, "reason=transaction-mismatch"));
    fixture.failure_code = -1;
    handle(&control, "cws-firmware-control/1 operation=APPLY command_id=a4 transaction_id=tx-c prepared_command_id=p3\n", reply);
    assert(strstr(reply, "reason=apply-failed") && cws_control_config_epoch(&control) == 1);
    fixture.failure_code = 0;
    handle(&control, "cws-firmware-control/1 operation=RESTORE command_id=r1 transaction_id=tx-c expected_config_epoch=1 prior_ping_hz=20\n", reply);
    assert(strstr(reply, "status=restored") && strstr(reply, "requested_ping_hz=20") &&
           cws_control_config_epoch(&control) == 2);
    handle(&control, "cws-firmware-control/1 operation=PREPARE command_id=p4 transaction_id=tx-d expected_config_epoch=2 ping_hz=30\n", reply);
    fixture.failure_code = -2;
    handle(&control, "cws-firmware-control/1 operation=APPLY command_id=a5 transaction_id=tx-d prepared_command_id=p4\n", reply);
    assert(strstr(reply, "reason=apply-state-uncertain"));
    fixture.failure_code = 0;
    {
        char tiny[8];
        assert(cws_control_handle(&control,
                                  "cws-firmware-control/1 operation=GET_STATE command_id=tiny transaction_id=tiny\n",
                                  strlen("cws-firmware-control/1 operation=GET_STATE command_id=tiny transaction_id=tiny\n"),
                                  tiny, sizeof(tiny)) < 0);
    }
    {
        char overlong[CWS_CONTROL_MAX_LINE + 2];
        memset(overlong, 'x', sizeof(overlong));
        overlong[sizeof(overlong) - 1] = '\0';
        handle(&control, overlong, reply);
        assert(strstr(reply, "reason=malformed-request"));
    }
    puts("cws_control_protocol host tests: PASS");
    return 0;
}
