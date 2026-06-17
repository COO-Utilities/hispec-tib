# Human Review Required

This is the central owner-review list for current code-vs-doc mismatches,
source TODOs, and behavior decisions.

LLMs Agents: Do NOT change heading names in this file.

## Locked-down code

## PCB Validation
- [x] sort out FVOA ripple, is ok?
  - seems to be ~2.8 mV or about 1785 effective levels at 5V, thats fine to proceed with real calibration
- [ ] Keep an eye out MEMS loop and ADC loop timing overruns
  - Scheduler and MEMS timing-log changes in `44084f1` eliminated this warning
    pattern in follow-up lab testing; keep open for continued monitoring of
    real active-toggle `late_rise`/`skipped_rise` warnings or ADC overruns.
  - debug build see `timinglog`


## Command/API Mismatches

### LLM Resolved; Human Review Requested
- [ ] Verify photodiode throughput uses the nearest nominal-laser wavelength
  correction coefficient table; all coefficients are intentionally `1.0` until
  lab values are installed.
- [ ] Verify command schemas use `persist` as the only persistence request key,
  default it to false when absent, and reject old `persistent`/`store` spellings.
- [ ] Verify Maiman driver serial mismatches report
  `blocked_reason:"driver_identity_mismatch"`, block driver-backed laser setting
  programming, and can be resolved only by operator-updating
  `laser/settings.expected_serial`.


## Decisions To Make

## TODOs
- start atten with output 'M' fails with "attenuator calibration start failed" should fail with at least "bad argument" more specific desired.
  - go through all commands and verify that invaid arguments are rejected with an informative error message. a minimum of "bad argument" and, where simple, low-code, and general a "<arg> invalid, see catalog" 
- go through entire codebase and find all times reported in fractional sections or ms odds are the preponderance should become integer seconds
- when the noise "resets" to 0 it makes it look like the noise has randomly gone away. This is actually MORE concerning than the discontinuities that the reset may be attempting to address. It should be allowed to briefly grow and those changes reverted or replaced with a temporarary nan until data to make a new reading is avaialble.

- algorithmic/proceedural status (e.g. atten aotocalibration or throughput monitor autoranging notices) should be going to console AND mqtt and that was the whole point of a combined dispatch helper. this needs a app-wide reevaluation.

- investigate additionaly complexity of making "ok" only responses give get responses over serial this probably adds too much code as some might need waits/delays that come naturally if a user is at a serial console. but would be a quality of life user improvement
- test `pd/dark/<channel>` measurement, forced dark with optional `rms_mv`,
  lowest-dark reset, and dark-window persistence over reboot.
- Status needs to gain things we actually want.

- `app/src/maiman.h`: compare Maiman behavior against the referenced validation/test scripts.

- houskeeping laserbank and lasers ALL have several checks for a non-null work_q. This is just paranoia. The q is static and started by main and the 
  program cant exist without it. Centralize and elimiinate this so that future readers to not wonder if it could be null. The app SHOULD break (though safely) if it is.
- <xxx>_append_<yyy>_or_null are all coo_json scope and must be refactored there.

- should  yj_residual_rms_mv/hk_residual_rms_mv be renamed to *_smoothed_rms_mv

- FIX (check ALL python helpers for comand drift):
  - pcb.laserbank_heaster(mode='override_on')
    Traceback (most recent call last):
    File "/Users/jibailey/src/hispec-zephyr-mlang/.venv/lib/python3.13/site-packages/IPython/core/interactiveshell.py", line 3748, in run_code
    exec(code_obj, self.user_global_ns, self.user_ns)
      ~~~~^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "<ipython-input-28-6f0710221e92>", line 1, in <module>
      pcb.laserbank_heater(mode='override_on')
      ~~~~~~~~~~~~~~~~~~~~^^^^^^^^^^^^^^^^^^^^
    File "/Users/jibailey/src/hispec-zephyr-mlang/hispec-tib/tools/hispec_fibpcb.py", line 2493, in laserbank_heater
      return _dataclass_from(LaserBankHeater, self._request_json("laserbank/heater"))
         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/Users/jibailey/src/hispec-zephyr-mlang/hispec-tib/tools/hispec_fibpcb.py", line 1568, in _dataclass_from
      values = {name: mapping.get(name) for name in names if name in mapping}
             ^^^^^^^^^^^^^
TypeError: LaserBankHeater.__init__() missing 3 required positional arguments: 'ambient_valid', 'valid_temps', and 'stale_temps'

## Deferred Owner-Specified Capabilities
