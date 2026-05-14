# Implementation Audit

## Authority

`commands.md` documents intended command/API behavior. Current C source in
`app/src/command.c` is the implementation source of truth. This page compares
the two without silently changing either contract.

## Dispatch Model

MQTT requests are accepted under:

```text
cmd/hsfib-tib/req/#
```

The suffix after the request prefix is copied into `Command.key`.
`dispatch_command()` chooses the longest dispatch-table key that is either an
exact match or followed by `/`.

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
| `laserbank/poweron` | yes, side effect | yes |
| `laserbank/poweroff` | yes, side effect | yes |
| `laserbank/clearfaults` | yes, side effect | yes |
| `laserbank/heater` | yes | yes |
| `laser` | yes, currently key-shape mismatch | yes, currently key-shape mismatch |
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
cmd/hsfib-tib/resp/<key>
```

MQTT 5 `response_topic` overrides this default if it fits the fixed topic
buffer. MQTT 5 `correlation_data` up to 16 bytes is copied into a fixed static
buffer and echoed exactly in responses.

## Commands Documented but Not Implemented

- `temp` alarm set behavior
- Full `status` payload including nested IP/temp/PD/laser/atten/last-command data
- Full intended `laser`/`lasersettings` command behavior as described in `commands.md`

## Commands Implemented but Missing or Stale in `commands.md`

- `pd` dark-measurement actions and `pdsettings` are more detailed in code
  than many older notes.
- `laserbank/poweron`, `laserbank/poweroff`, and `laserbank/clearfaults`
  have current implementations but no driver fault-state integration.

## High-Risk Implementation Mismatches

- `laser` command key parsing appears inconsistent with dispatch. The dispatch
  entry is `laser`, which accepts `laser/...`, but the handler uses
  `parse_key_pair()` and then calls `get_laser_channel(laser_name + 5)`. For a
  request like `laser/1028y/current`, `laser_name` is `laser`, so the lookup is
  invalid. A key shaped like `laser1028y/current` would make the pointer math
  plausible, but it does not match the dispatch table.
- The local `laser_t` enum maps both `LASER_1028_Y` and `LASER_1270_J` to
  channel value 1. This affects attenuator and laser mapping review.
- Laser-bank power actions are registered for both GET and SET. Empty MQTT or
  serial queries to those exact keys therefore perform power actions.
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
