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


## Decisions To Make

- Decide intended persistence for MEMS switch state, AS split requested/actual state and last-command metadata.
  - This involves looking at optical stability of repeated pulses to the same state

## TODOs

- Add command to enumerate route names, input and output names, laser names
- accept a and b as aliases of A and B for switch states
- duty cycle .3412129 with requested_toggle_rate_hz=1000 (bad) quantizes to a reported value of "0.3" not ok, loss of resolution

laserbank/power returns 
        { "mode": "override_off", "powered": false }
but accepts only override=, change to mode=

- bank power mode in auto  does not autopower  for a laser_status query, I guess that makes sense, but not for things like serial query

- bank power in auto does not autopower for setting a laser setting:
  - If the laser bank is off, firmware powers it, applies driver-backed settings, verifies them as practical, and then restores the previous bank power state. If laserbank/power is override_off, driver-backed settings changes return an error.
  - and yet:
    - pcb.set_laser_settings('2330k', default_operating_temp_c=25)
      Out[26]: CommandOk(status='ok')
- yj_rms_mv_0p5s is reporting 0.0 yet "yj_noise_rms_mv": 0.041  when there is a real photodiode connected, I suspect a bug.
- photodiode defaults are set in app_settings.c and not in devices or photodiode this is both inconsisten with laser properties and hard to find. it needs to move to the photodiode header THAT is the first place people will look.
- python gets dark via pdsettings
- pdsettings includes:
  - average='complete', average_duration_ms=2000, average_samples=100, average_target_samples=100, that are all likly for dark measurement
  - replace with only dark_duration_ms=# or dark_duration_ms='user' (for if it was set by the user) do not record user dark values into lowest seen
  - drop lowest_dark_valid as after the very first recording it will always be valid
- after measuring dark with no persistence "lowest_dark_valid" still says false and docs do not describe what it is after measuring with store=true lowest_dark_valid=true, rename to lowest_stored_dark_mv, drop lowest_dark_valid, and replace with nan if it isn't valid, add a cell for the analytical noise on the dark per the noise chain and number of samples, and adc sample rate 

-todo test dark settings and persistence but  noise_rms_mV works and does persistence over reboots no reason to suspect others don't

- pcb.set_serialguard should be allowable while active and update expiry time (otherwise someone could be waiting a LOOOONG time)

- status needs to gain things we actually want

- change heater last_poll_age_ms to poll_age_s
  - heater_mode to mode
  - override to mode (parallelis with laserbank)
  - heater stayed on at room temp with no laser temps valid after oging from override_on to auto
  - meaning of any_disabled_below_15c and any_disabled_above_off_threshold not documented
  - reporting of "all_tecs_enabled": false, "all_tecs_enabled_ms": 0 seems out of place probably axe, maybe keep but refactor packaging so relavance is clear

- laser needs some thought around serial its unsettability and serial ok, maybe going ok on the first boot after a serial change (cause it gets saved?)

- im seeing signs that laser commands can fail if the periodic laser temp polling is happening
  - [01:04:10.613,000] <inf> coo_mqtt: topic: 'cmd/hsfib-tib/req/laser/engstatus', payload: {"name":"2330k"}
    [01:04:10.613,000] <inf> coo_command_dispatch: Dispatching: laser/engstatus
    [01:04:10.623,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.623,000] <err> maiman: Modbus read node=2 reg=0x0001 failed: -116
    [01:04:10.633,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.633,000] <err> maiman: Modbus read node=2 reg=0x0003 failed: -116
    [01:04:10.643,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.643,000] <err> maiman: Modbus read node=2 reg=0x0004 failed: -116
    [01:04:10.653,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.653,000] <err> maiman: Modbus read node=2 reg=0x007a failed: -116
    [01:04:10.663,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.663,000] <err> maiman: Modbus read node=2 reg=0x0005 failed: -116
    [01:04:10.673,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.673,000] <err> maiman: Modbus read node=2 reg=0x0008 failed: -116
    [01:04:10.683,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.683,000] <err> maiman: Modbus read node=2 reg=0x0040 failed: -116
    [01:04:10.693,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.693,000] <err> maiman: Modbus read node=2 reg=0x0024 failed: -116
    [01:04:10.704,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.704,000] <err> maiman: Modbus read node=2 reg=0x0025 failed: -116
    [01:04:10.714,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.714,000] <err> maiman: Modbus read node=2 reg=0x0029 failed: -116
    [01:04:10.724,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.724,000] <err> maiman: Modbus read node=2 reg=0x002a failed: -116
    [01:04:10.734,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.734,000] <err> maiman: Modbus read node=2 reg=0x0088 failed: -116
    [01:04:10.744,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.744,000] <err> maiman: Modbus read node=2 reg=0x0041 failed: -116
    [01:04:10.754,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.754,000] <err> maiman: Modbus read node=2 reg=0x0070 failed: -116
    [01:04:10.764,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.764,000] <err> maiman: Modbus read node=2 reg=0x0075 failed: -116
    [01:04:10.774,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.774,000] <err> maiman: Modbus read node=2 reg=0x0043 failed: -116
    [01:04:10.784,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.784,000] <err> maiman: Modbus read node=2 reg=0x0076 failed: -116
    [01:04:10.794,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.794,000] <err> maiman: Modbus read node=2 reg=0x0077 failed: -116
    [01:04:10.805,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.805,000] <err> maiman: Modbus read node=2 reg=0x0078 failed: -116
    [01:04:10.815,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.815,000] <err> maiman: Modbus read node=2 reg=0x008a failed: -116
    [01:04:10.825,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:10.825,000] <err> maiman: Modbus read node=2 reg=0x0091 failed: -116
    [01:04:16.420,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:16.420,000] <err> maiman: Modbus read node=5 reg=0x0075 failed: -116
    [01:04:16.430,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:16.430,000] <err> maiman: Modbus read node=5 reg=0x007a failed: -116
    [01:04:16.440,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:16.440,000] <err> maiman: Modbus read node=6 reg=0x0075 failed: -116
    [01:04:16.450,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:16.450,000] <err> maiman: Modbus read node=6 reg=0x007a failed: -116
    [01:04:16.460,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:16.460,000] <err> maiman: Modbus read node=4 reg=0x0075 failed: -116
    [01:04:16.470,000] <wrn> modbus: Client wait-for-RX timeout
    [01:04:16.470,000] <err> maiman: Modbus read node=4 reg=0x007a failed: -116
    [01:04:16.481,000] <wrn> modbus: Client wait-for-RX timeout
  - here channel 2 is online, the rest have no devices

- `app/src/maiman.h`: compare Maiman behavior against the referenced validation/test scripts.

## Deferred Owner-Specified Capabilities
