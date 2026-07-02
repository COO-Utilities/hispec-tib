# Implementation Audit

## Authority

`commands.md` documents intended command/API behavior. The dispatcher built-ins
in `lib/coo_commons/command_dispatch.c`, the app command spec table in
`app/src/command.c`, and the current command handlers are the implementation
source of truth. This page compares the two without silently changing either
contract.

## Dispatch Model

MQTT requests are accepted under:

```text
cmd/<device>/req/#
```

The suffix after the request prefix is copied into `Command.key`.
`coo_cmd_runtime_find_spec()` chooses the longest command-spec key that is
either an exact match or followed by `/`; command dispatch then applies default
support checks, handler selection, lastcommand recording, and built-in reboot
pending rejection unless an app override execute callback is configured. The
`<device>` component is board-profile dependent: `hsfib-tib`, `hsfib-rcal`,
`hsfib-bcal`, or `hsfib-as`.

Dispatcher built-ins:

| Entry | Query handler | Effect/action handler | Notes |
| --- | --- | --- | --- |
| `help` | yes | no | Serial prints directly; MQTT returns compact endpoints. |
| `serialguard` | yes | yes | Present when `CONFIG_COO_CMD_SERIAL_GUARD` is enabled. |
| `reboot` | no | yes | Present when `CONFIG_COO_CMD_REBOOT` is enabled. |

Implemented app dispatch entries. The column names reflect internal C dispatch
slots; the external API is documented as queries, effect requests, and actions.

| Entry | Query handler | Effect/action handler |
| --- | --- | --- |
| `ip` | yes | yes |
| `mqtt` | yes | yes |
| `time` | yes | yes |
| `memsroute` | yes | yes |
| `memsroute/route_loss` | yes | yes |
| `mems` | yes | yes |
| `split` | yes | yes |
| `measure_throughput` | no | yes |
| `laserbank/power` | yes | yes |
| `laserbank/clearfaults` | no | yes |
| `laserbank/heater` | yes | yes |
| `laser/status` | yes | no |
| `laser/settings` | yes | yes |
| `laser/tune` | yes | yes |
| `laser` | yes | yes |
| `atten` | yes | yes |
| `pdsettings` | yes | yes |
| `pd` | yes | yes |
| `temp` | yes | no |
| `status` | yes | no |

## Request Classification

MQTT and serial are normalized to a shared `Command` and then classified by
command dispatch using the app command spec table. The internal result still
uses `MSG_GET` and `MSG_SET`, but those names are dispatch-slot names, not
user-visible protocol verbs. Serial `help` is the exception: it prints directly
from command dispatch before entering the inbound queue.

Empty/no-payload requests are queries except:

- `reboot`
- `laserbank/clearfaults`
- topic-suffix `laserbank/power/<mode>`
- topic-suffix `laserbank/heater/<mode>`

Non-empty payload requests are effect/action requests except documented
payload-query shapes:

- `status`
- `laser/status`
- `memsroute/route_loss` when the payload contains only `route`
- `laser` when `level` is absent
- `laser/tune` when `tune_nm` and `delta_nm` are absent
- `laser/settings` when the nested `settings` object is absent

The old MQTT `msg_type` payload convention is not used by command ingress.

## Response Rules

Current command responses follow the global `commands.md` contract:

- Data-less success: `{"status":"ok"}`.
- Data-bearing success: response data only, no top-level transport `status`.
- Failure: an `error` key, with optional diagnostics such as `rc`.

A source search of `command.c` has no remaining literal old-style command
responses of the forms `{"status":"success"}`,
`{"status":"error","msg":...}`, `{"status":"OK"}`, or partial transport
`status`.

## Response Topics

The default MQTT response topic is:

```text
cmd/<device>/resp/<key>
```

MQTT 5 `response_topic` overrides this default if it fits the fixed topic
buffer. MQTT 5 `correlation_data` up to 16 bytes is copied into a fixed static
buffer and echoed exactly in responses.

## Commands Documented but Not Fully Implemented

- None known after this audit pass.

## Commands Implemented but Missing or Stale in `commands.md`

- None known.

## High-Risk Implementation Notes

- `status` optional `lasers` and `attens` sections can perform Modbus and DAC
  reads. A large optional response can fail with `{"error":"status response too large"}`
  if it exceeds the fixed MQTT payload buffer.
- `lastcommand` records supported effect-capable requests and is persisted by
  command dispatch through a fixed NVS record configured by the app. Pure query
  requests are not recorded.
- Serial `help` depends on help metadata attached to the app command spec table.
  It now reflects the code paths reviewed in this audit and uses generic support
  predicates to mark unsupported commands, but future command behavior changes
  must update the spec help metadata or help can become stale.
- `laserbank/clearfaults` occupies both internal dispatch slots for legacy
  reasons, but ingress classifies no-payload requests as an action.

## Blocking and Queueing Summary

- Dispatcher built-ins run in command dispatch. Serial `help` prints directly;
  MQTT `help`, `serialguard`, and `reboot` enqueue immediate responses.
- App command handlers run in the single command executor thread.
- App responses are enqueued to `outbound_queue` and published or printed later.
- Attenuator commands can block on DAC I2C.
- Laser and Maiman commands can block on Modbus RTU and laser-bank boot sleeps.
- Persistent settings updates can block on Zephyr NVS writes.
- Lastcommand persistence can block on Zephyr NVS writes before an effect
  handler runs.
- `status` optional laser/attenuator sections can block on Modbus/DAC reads.
- MEMS and split commands update router state and can enqueue warnings but do
  not publish directly.
- Warning publication is best-effort through `outbound_queue`.
- Throughput telemetry is best-effort and can be dropped under MQTT or queue
  backpressure.
