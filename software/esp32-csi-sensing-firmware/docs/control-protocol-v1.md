# `cws-firmware-control/1`

This is the bounded USB-serial control protocol implemented by the firmware.
It controls only the firmware-owned ping probe rate; it never changes the AP,
Wi-Fi channel, BSSID, SSID, or other WLAN setting.  `CWS_REBOOT` remains the
separate legacy, deliberate fault-injection command.

## Frame

One request and one reply occupy one ASCII line, terminated by `LF` (an
optional preceding `CR` is accepted). A complete input frame is at most 256
bytes including the line terminator. Tokens are one literal protocol token and
space-separated `key=value` pairs. Values do not contain whitespace or `=`.
Unknown, duplicate, malformed, non-ASCII, or overlong fields reject the frame;
the parser does not infer a partial command.

```
cws-firmware-control/1 operation=PREPARE command_id=prepare-01 transaction_id=txn-01 expected_config_epoch=4 ping_hz=20
```

`command_id`, `transaction_id`, and `prepared_command_id` are 1--48 ASCII
letters/digits plus `.`, `_`, or `-`. `command_id` is unique within the current
boot. A `transaction_id` is stable across its `PREPARE`, `APPLY`, `QUERY`, and
`RESTORE` phases. All rates are decimal integers in `0..50`; zero stops only
the generated probe session.

Every reply has the following base fields:

```
cws-firmware-control/1 operation=<lower-kebab> command_id=<id> transaction_id=<id> status=<status> reason=<lower-kebab> boot_epoch=<u32> config_epoch=<u32> effective_ping_hz=<0..50> active=<0|1>
```

`CAPABILITIES`, `GET_STATE`, and `QUERY` add read-only facts supplied by the
firmware: profile/version and node identity, supported `rate_min_hz=0` and
`rate_max_hz=50`, and only already-observed band/channel/BSSID information.
`CAPABILITIES` and `GET_STATE` also return one lock-consistent bounded snapshot
of `csi_accepted`, `csi_invalid`, `output_drops`, `ping_success`, and
`ping_timeouts`.
The endpoint never treats those facts as writable configuration.

## Operations

| Operation | Exact permitted extra fields | Effect |
| --- | --- | --- |
| `CAPABILITIES` | none | Read-only capability and identity response. |
| `GET_STATE` | none | Read-only current state response. |
| `QUERY` | `query_command_id` | Recovery primitive for a lost reply: returns the exact cached terminal reply for that command only when the transaction matches. It never invokes a callback; an evicted or unknown target returns `unknown-command`. |
| `PREPARE` | `expected_config_epoch`, `ping_hz` | Validates the epoch and requested rate, then retains a bounded prepare record. No callback or configuration mutation occurs. |
| `APPLY` | `prepared_command_id` | Resolves the matching prepared command and transaction. It calls the rate callback once. A successful rate change increments `config_epoch` exactly once; an already-effective rate does not increment it. |
| `RESTORE` | `expected_config_epoch`, `prior_ping_hz` | Applies an explicit prior rate only if the epoch is current. A successful rate change increments `config_epoch` once. |

The matrix is strict: a known field that is not listed for the operation is
rejected with `inapplicable-field`; it is never silently omitted from canonical
idempotency semantics. `PREPARE`, `APPLY`, and `RESTORE` always add
`requested_ping_hz` before `effective_ping_hz`. For `APPLY` that requested value
is recovered from its prepared record; an expired/unknown prepare reports
`requested_ping_hz=unknown` with its rejection.

Reasons are stable lower-kebab values, including `malformed-request`,
`command-id-conflict`, `stale-config-epoch`, `unknown-prepare`, `unknown-command`,
`transaction-mismatch`, `apply-failed`, `apply-state-uncertain`, `restore-failed`,
and `restore-state-uncertain`. A normal rejected frame or callback failure is
fail-closed and retains its prior effective rate and epoch. The explicitly
named `*-state-uncertain` exception means the firmware has lost the prior ping
session as well: it truthfully synchronizes effective rate to `0` and advances
`config_epoch`, so a controller must recover from the emitted state rather than
assuming rollback succeeded.

## Idempotence and bounds

The idempotency key is `(boot_epoch, command_id, canonical-request)`. Exact
duplicates return the cached reply and never call the rate callback again. A
reused `command_id` with any other canonical request returns
`command-id-conflict`. Replay and prepared records are RAM-bounded and reset at
boot; callers must use a fresh command ID after their retention window or a
reboot. `config_epoch` likewise starts at zero for each boot and is emitted in
the profile and heartbeat telemetry, so controller/collector correlation does
not rely on wall-clock time. The raw CSI CSV schemas stay unchanged for
collector compatibility; the epoch is correlated through those separate,
serialized profile/heartbeat/control records. Firmware callback success proves
only local API success. The collector still verifies a postcondition through a
matching state/heartbeat record before committing an actuator transaction.
