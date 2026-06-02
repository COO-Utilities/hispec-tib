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

- `app/src/maiman.h`: compare Maiman behavior against the referenced validation/test scripts.

## Deferred Owner-Specified Capabilities
