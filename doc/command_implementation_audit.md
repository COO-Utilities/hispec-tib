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
exact match or followed by `/`.
The `<device>` component is board-profile dependent: `hsfib-tib`,
`hsfib-rcal`, `hsfib-bcal`, or `hsfib-as`.

Implemented dispatch entries:

| Entry | GET handler | SET handler |
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
| `laserbank/clearfaults` | yes, conditional side effect | yes, conditional side effect |
| `laserbank/heater` | yes | yes |
| `laser` | yes | yes |
| `laser/tune` | yes | yes |
| `laser/status` | yes | no |
| `laser/engstatus` | yes | no |
| `laser/settings` | yes | yes |
| `atten` | yes | yes |
| `pdsettings` | yes | yes |
| `pd` | yes | yes |
| `temp` | yes | no |
| `status` | yes | no |

## GET and SET Selection

MQTT behavior:

- Empty payload means GET.
- Non-empty payload means SET unless JSON contains `msg_type:"get"`.
- A non-empty GET payload therefore requires `msg_type:"get"`.

Serial behavior:

- `<key>` means GET.
- `<key> <payload>` means SET.
- There are no serial `get` or `set` words.
- Non-JSON shorthand is normalized to JSON before dispatch.

This creates a mismatch with `commands.md` for commands that document optional
GET payloads, such as `pd` with `{"unit":"volts"}`. In current firmware that
payload is a SET unless `msg_type:"get"` is also present.

## Response Topics

The default MQTT response topic is:

```text
cmd/<device>/resp/<key>
```

MQTT 5 `response_topic` overrides this default if it fits the fixed topic
buffer. MQTT 5 `correlation_data` up to 16 bytes is copied into a fixed static
buffer and echoed exactly in responses.

## Commands Documented but Not Implemented

- Full `status` payload including nested IP/temp/PD/laser/atten/last-command data

## Commands Implemented but Missing or Stale in `commands.md`

- `pd` dark-measurement actions and `pdsettings` are more detailed in code
  than many older notes.

## High-Risk Implementation Mismatches

- `reboot` is SET-only. An empty MQTT payload or bare serial `reboot` is
  unsupported; a non-empty payload schedules a reboot.

## Blocking and Queueing Summary

- All command handlers run in the single command executor thread.
- Responses are enqueued to `outbound_queue` and published or printed later.
- Attenuator commands can block on DAC I2C.
- Laser and Maiman commands can block on Modbus RTU and laser-bank boot sleeps.
- Settings updates can block on the Zephyr settings backend.
- MEMS and split commands update router state and can enqueue warnings but do
  not publish directly.
- Warning publication is best-effort through `outbound_queue`.
- Throughput telemetry is best-effort and can be dropped under MQTT or queue
  backpressure.
