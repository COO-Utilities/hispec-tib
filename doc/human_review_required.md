# Human Review Required

This is the central owner-review list for current code-vs-doc mismatches,
source TODOs, and behavior decisions.

LLMs Agents: Do NOT change heading names in this file.

## PCB Validation
- [x] Verify boot MEMS switch state behavior.
  - .15ms pulse A+B simultaneously at reboot then pulses A on all simultaneously about 6s later. is this ok?
- [x] sort out FVOA ripple, is ok?
  - seems to be ~2.8 mV or about 1785 effective levels at 5V, thats fine to proceed with real calibration
- [ ] Keep an eye out MEMS loop and ADC loop timing overruns
  - Scheduler and MEMS timing-log changes in `44084f1` eliminated this warning
    pattern in follow-up lab testing; keep open for continued monitoring of
    real active-toggle `late_rise`/`skipped_rise` warnings or ADC overruns.
  - debug build see `timinglog`
  - non-debug build:
    - [00:00:35.747,000] <inf> sntp_sync: SNTP sync complete (manual: 132.163.96.4)
      [00:01:12.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:01:52.619,000] <wrn> photodiode: ADC timing: samples=500 missed=0 overruns=1 settings_busy=0 worst_loop_us=20046 min_margin_us=-46 worst_non_adc_us=81 adc_floor_us=4340 adc_us=4571/15394 adc_over_us=231/11054 errors=0/0
      [00:02:12.619,000] <wrn> photodiode: ADC timing: samples=500 missed=0 overruns=1 settings_busy=0 worst_loop_us=20312 min_margin_us=-312 worst_non_adc_us=80 adc_floor_us=4340 adc_us=15708/4524 adc_over_us=11368/184 errors=0/0
      [00:02:42.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:03:42.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:05:12.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:06:02.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:06:52.619,000] <wrn> photodiode: ADC timing: samples=500 missed=0 overruns=1 settings_busy=0 worst_loop_us=20146 min_margin_us=-146 worst_non_adc_us=80 adc_floor_us=4340 adc_us=4570/15496 adc_over_us=230/11156 errors=0/0
      [00:07:12.619,000] <wrn> photodiode: ADC timing: samples=500 missed=0 overruns=1 settings_busy=0 worst_loop_us=20115 min_margin_us=-115 worst_non_adc_us=79 adc_floor_us=4340 adc_us=15508/4528 adc_over_us=11168/188 errors=0/0
      [00:07:42.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:08:32.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:10:12.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:11:02.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:11:42.619,000] <wrn> photodiode: ADC timing: samples=500 missed=0 overruns=1 settings_busy=0 worst_loop_us=20045 min_margin_us=-45 worst_non_adc_us=81 adc_floor_us=4340 adc_us=4568/15396 adc_over_us=228/11056 errors=0/0
      [00:12:02.619,000] <wrn> photodiode: ADC timing: samples=500 missed=0 overruns=1 settings_busy=0 worst_loop_us=20115 min_margin_us=-115 worst_non_adc_us=79 adc_floor_us=4340 adc_us=15509/4527 adc_over_us=11169/187 errors=0/0
      [00:12:42.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:13:22.619,000] <wrn> photodiode: ADC timing: samples=500 missed=0 overruns=1 settings_busy=0 worst_loop_us=20416 min_margin_us=-416 worst_non_adc_us=79 adc_floor_us=4340 adc_us=4570/15767 adc_over_us=230/11427 errors=0/0
      [00:13:32.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:14:12.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:15:02.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:16:02.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:16:42.619,000] <wrn> photodiode: ADC timing: samples=500 missed=0 overruns=1 settings_busy=0 worst_loop_us=20079 min_margin_us=-79 worst_non_adc_us=81 adc_floor_us=4340 adc_us=4570/15428 adc_over_us=230/11088 errors=0/0
      [00:17:02.619,000] <wrn> photodiode: ADC timing: samples=500 missed=0 overruns=1 settings_busy=0 worst_loop_us=20311 min_margin_us=-311 worst_non_adc_us=81 adc_floor_us=4340 adc_us=15706/4524 adc_over_us=11366/184 errors=0/0
      [00:17:32.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:18:22.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:19:12.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:20:02.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:22:32.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:23:12.619,000] <wrn> photodiode: ADC timing: samples=500 missed=0 overruns=1 settings_busy=0 worst_loop_us=20146 min_margin_us=-146 worst_non_adc_us=81 adc_floor_us=4340 adc_us=4570/15495 adc_over_us=230/11155 errors=0/0
      [00:23:32.619,000] <wrn> photodiode: ADC timing: samples=500 missed=0 overruns=1 settings_busy=0 worst_loop_us=20312 min_margin_us=-312 worst_non_adc_us=80 adc_floor_us=4340 adc_us=15708/4524 adc_over_us=11368/184 errors=0/0
      [00:24:02.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0
      [00:24:52.617,000] <wrn> mems_switching: MEMS timing: events=1000 missed_base=1 late_service=0 late_pulse=0 stale_skips=0 cleanup_late=0 worst_base_miss=1 worst_late_cycles=0 worst_cleanup_ms=0


## Command/API Mismatches

### LLM Resolved; Human Review Requested

- [ ] Verify MEMS and split non-constant duty responses now report actual
  `cycle_ms`, `a_ms`, `b_ms`, and `stop_in_s` timing fields.
- [ ] Verify `laserbank/heater` now reports compact algorithm-facing status
  without the old wordy `all_tecs_enabled` / `any_disabled_below_15c` fields.
- [ ] Verify heater `auto` mode turns the heater off when all laser temperatures
  are stale and ambient is invalid or at least 15 C.
- [ ] Verify `pcb.set_serialguard(seconds)` no longer sends `persistent:false`
  and can update expiry while serial guard is active.
- [ ] Verify `laser/settings` temporarily powers the bank for driver-backed
  updates and documents that `default_operating_temp_c` is the TEC start
  setpoint.
- [ ] Verify `pdsettings` reports `dark_duration_ms`, `dark_noise_rms_mv`, and
  `lowest_stored_dark_mv` without exposing `lowest_dark_valid`.
- [ ] Verify photodiode throughput uses the nearest nominal-laser wavelength
  correction coefficient table; all coefficients are intentionally `1.0` until
  lab values are installed.
- [ ] Verify command dispatch now rejects unknown top-level payload keys and
  malformed non-parameterized endpoint keys such as `laser/angstatus`.
- [ ] Verify `laser name=1028y value=50` now reports an unknown `value`
  argument, and `laser ... level=...` reports `override_off` when bank power is
  explicitly disabled.
- [ ] Verify compact laser status reports `ready`/`blocked_reason` and uses
  integer-or-null active time fields instead of inactive `0.0` values.
- [ ] Verify the duplicate laser-bank fault-clear timeout define was removed
  from `laser_command.c`; broader redundant-setting cleanup remains open.


## Decisions To Make

- Decide intended persistence for MEMS switch state, AS split requested/actual state and last-command metadata.
  - This involves looking at optical stability of repeated pulses to the same state

- Static attens:
  - # ### Anticipated Static Attenuation Chains
    # - 1028: -90.0 to -73.0 dB, range 17.0 dB, **static -73.0 dB**
    # - 1270:  -70.0 to -40.0 dB, range 30.0 dB, **static -40.0 dB**
    # - 1430: range 0.0 dB, **static -100.0 dB**
    # - 1510: -80.0 to -33.0 dB, range 47.0 dB, **static -33.0 dB**
    # - 2330: -73.0 to -3.0 dB, range 70.0 dB, **static -3.0 dB**

- "blocked_reason": "not_emitting", should ne "N/A (not_emittting" or some such

## TODOs

- yj_rms_mv_0p5s is reporting 0.0 yet "yj_noise_rms_mv": 0.041  when there is a real photodiode connected, I suspect a bug.
- TODO: test dark settings and persistence, especially `dark_duration_ms`,  `dark_noise_rms_mv`, and `lowest_stored_dark_mv` over reboot.
- Status needs to gain things we actually want.
- Laser needs some thought around serial its unsettability and serial ok, maybe going ok on the first boot after a serial change (cause it gets saved?)
- Laserbank query's now work for all lasers, sometimes.
  - temps now has the idle tec temps in it, so they are sucessfully being polled
  - but I sill see regular timeouts on specific registers:
   [00:16:01.941,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:01.941,000] <err> maiman: Modbus read node=5 reg=0x0075 failed: -116
    [00:16:01.951,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:01.951,000] <err> maiman: Modbus read node=5 reg=0x007a failed: -116
    [00:16:01.961,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:01.961,000] <err> maiman: Modbus read node=6 reg=0x0075 failed: -116
    [00:16:01.971,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:01.971,000] <err> maiman: Modbus read node=6 reg=0x007a failed: -116
    [00:16:01.981,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:01.981,000] <err> maiman: Modbus read node=4 reg=0x0075 failed: -116
    [00:16:01.991,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:01.991,000] <err> maiman: Modbus read node=4 reg=0x007a failed: -116
    [00:16:02.001,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:02.001,000] <err> maiman: Modbus read node=3 reg=0x0075 failed: -116
    [00:16:02.011,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:02.011,000] <err> maiman: Modbus read node=3 reg=0x007a failed: -116
    [00:16:02.021,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:02.021,000] <err> maiman: Modbus read node=1 reg=0x0075 failed: -116
    [00:16:02.031,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:02.031,000] <err> maiman: Modbus read node=1 reg=0x007a failed: -116
    [00:16:02.042,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:02.042,000] <err> maiman: Modbus read node=2 reg=0x0075 failed: -116
    [00:16:02.052,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:02.052,000] <err> maiman: Modbus read node=2 reg=0x007a failed: -116
    [00:16:12.500,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.500,000] <err> maiman: Modbus read node=5 reg=0x0075 failed: -116
    [00:16:12.510,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.510,000] <err> maiman: Modbus read node=5 reg=0x007a failed: -116
    [00:16:12.520,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.520,000] <err> maiman: Modbus read node=6 reg=0x0075 failed: -116
    [00:16:12.530,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.530,000] <err> maiman: Modbus read node=6 reg=0x007a failed: -116
    [00:16:12.540,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.540,000] <err> maiman: Modbus read node=4 reg=0x0075 failed: -116
    [00:16:12.550,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.550,000] <err> maiman: Modbus read node=4 reg=0x007a failed: -116
    [00:16:12.560,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.560,000] <err> maiman: Modbus read node=3 reg=0x0075 failed: -116
    [00:16:12.571,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.571,000] <err> maiman: Modbus read node=3 reg=0x007a failed: -116
    [00:16:12.581,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.581,000] <err> maiman: Modbus read node=1 reg=0x0075 failed: -116
    [00:16:12.591,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.591,000] <err> maiman: Modbus read node=1 reg=0x007a failed: -116
    [00:16:12.601,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.601,000] <err> maiman: Modbus read node=2 reg=0x0075 failed: -116
    [00:16:12.611,000] <wrn> modbus: Client wait-for-RX timeout
    [00:16:12.611,000] <err> maiman: Modbus read node=2 reg=0x007a failed: -116


- `app/src/maiman.h`: compare Maiman behavior against the referenced validation/test scripts.

## Deferred Owner-Specified Capabilities
