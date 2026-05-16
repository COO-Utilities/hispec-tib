# Implementation Audit

## Authority

`commands.md` documents intended command/API behavior. Current C source in
`app/src/command.c` is the implementation source of truth. This page compares
the two without silently changing either contract.

## Dispatch Model

MQTT requests are accepted under:

```text
cmd/<device>/req/#
```

The suffix after the request prefix is copied into `Command.key`.
`dispatch_command()` chooses the longest dispatch-table key that is either an
exact match or followed by `/`. The `<device>` component is board-profile
dependent: `hsfib-tib`, `hsfib-rcal`, `hsfib-bcal`, or `hsfib-as`.

Implemented dispatch entries:

| Entry | GET handler | SET/action handler |
| --- | --- | --- |
| `help` | yes | no |
| `ip` | yes | yes |
| `mqtt` | yes | yes |
| `time` | yes | yes |
| `reboot` | no | yes |
| `serialguard` | yes | yes |
| `memsroute` | yes | yes |
| `memsroute/route_loss` | yes | yes |
| `mems` | yes | yes |
| `split` | yes | yes |
| `measure_throughput` | no | yes |
| `laserbank/power` | yes | yes |
| `laserbank/clearfaults` | yes, same action handler | yes, same action handler |
| `laserbank/heater` | yes | yes |
| `laser/engstatus` | yes | no |
| `laser/status` | yes | no |
| `laser/settings` | yes | yes |
| `laser/tune` | yes | yes |
| `laser` | yes | yes |
| `atten` | yes | yes |
| `pdsettings` | yes | yes |
| `pd` | yes | yes |
| `temp` | yes | no |
| `status` | yes | no |

## GET and SET Selection

MQTT and serial are normalized to a shared `Command` and then classified by
`command_infer_msg_type()`.

Empty/no-payload requests are GET except:

- `reboot`
- `laserbank/clearfaults`
- topic-suffix `laserbank/power/<mode>`
- topic-suffix `laserbank/heater/<mode>`

Non-empty payload requests are SET/action except documented payload-query
shapes:

- `status`
- `laser/status`
- `laser/engstatus`
- `memsroute/route_loss` when `laser` is present
- `laser` when `level` is absent
- `laser/tune` when `tune_nm` and `delta_nm` are absent
- `laser/settings` when the nested `settings` object is absent

The old MQTT `msg_type:"get"` convention is not used by command ingress.

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

- `help` is documented as command summary plus device info. The implementation
  returns only a static summary string and does not enumerate every suffix
  endpoint.

## Commands Implemented but Missing or Stale in `commands.md`

- None known.

## High-Risk Implementation Notes

- `status` optional `lasers` and `attens` sections can perform Modbus and DAC
  reads. A large optional response can fail with `{"error":"status response too large"}`
  if it exceeds the fixed MQTT payload buffer.
- `status` updates `lastcommand` before dispatch, so a status query reports
  itself as the last command.
- Serial guard is stricter than the prose for some GET-shaped commands:
  `mqtt_get_allowed_during_serial_guard()` blocks all `laserbank/*` and `laser`
  GETs while serial guard is active.
- `laserbank/clearfaults` is both GET and SET in the dispatch table for legacy
  reasons, but ingress classifies no-payload requests as SET/action.

## Blocking and Queueing Summary

- All command handlers run in the single command executor thread.
- Responses are enqueued to `outbound_queue` and published or printed later.
- Attenuator commands can block on DAC I2C.
- Laser and Maiman commands can block on Modbus RTU and laser-bank boot sleeps.
- Settings updates can block on the Zephyr settings backend.
- `status` optional laser/attenuator sections can block on Modbus/DAC reads.
- MEMS and split commands update router state and can enqueue warnings but do
  not publish directly.
- Warning publication is best-effort through `outbound_queue`.
- Throughput telemetry is best-effort and can be dropped under MQTT or queue
  backpressure.
