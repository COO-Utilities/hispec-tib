# CLion Prompt Library Templates (HiSPEC-TIB)

Copy/paste templates for repeated tasks.

## Prompt: Overlay Sync Pass
Review `app/doc/hardware.md` and `app/boards/nucleo_h563zi.overlay` for mismatches.
Rules:
1. `hardware.md` is source of truth.
2. Do not edit `hardware.md` unless I explicitly confirm.
3. Report findings first with file:line references.
4. Then patch overlay only.
5. Run west build for `nucleo_h563zi/stm32h563xx` and report result.

## Prompt: Linker Unresolved Symbol Triage
I have a linker error. Diagnose and patch minimally.
Rules:
1. Identify exact undefined symbol and call site.
2. Check declaration vs definition and linkage (`static` vs external).
3. Prefer the smallest fix that preserves existing API shape.
4. Do not refactor unrelated code.
5. Rebuild and report first blocking error or success.

## Prompt: AI Self Review (Pre-Commit)
Review all modified files as a code reviewer.
Output format:
1. Findings (highest severity first) with file:line.
2. Regression risks.
3. Missing tests/build validations.
4. A short patch plan.
Constraints:
- Focus on behavioral correctness and integration risk.
- Flag doc/code drift explicitly.

## Prompt: West Environment Sanity Check
Validate this workspace can build from CLion terminal.
Tasks:
1. Print exact `west` binary path and python venv used.
   - If `west` is not on PATH, use `/Users/jibailey/zephyrproject/.venv/bin/west`.
2. Confirm Zephyr SDK/toolchain resolution.
3. Run one clean Nucleo build with explicit `--build-dir`.
4. Summarize reproducible command for this machine.

## Prompt: Canonical Build Command Rule
When validating code changes for this app, use this exact west binary and command shape unless explicitly told otherwise:
`/Users/jibailey/zephyrproject/.venv/bin/west build --cmake-only --board=nucleo_h563zi/stm32h563xx --build-dir /Users/jibailey/src/hispec-zephyr-mlang/hispec-tib/app/build /Users/jibailey/src/hispec-zephyr-mlang/hispec-tib/app -- -DCMAKE_MAKE_PROGRAM=/Users/jibailey/Applications/CLion.app/Contents/bin/ninja/mac/aarch64/ninja`
Then run the same command without `--cmake-only` for a full compile check.
Always run these two commands sequentially (not in parallel) when using the same `--build-dir`.

## Prompt: Hardware-to-DTS Mapping Update
I changed hardware wiring. Update DTS overlay accordingly.
Tasks:
1. Parse mapping from `app/doc/hardware.md`.
2. Update `app/boards/nucleo_h563zi.overlay` only.
3. Keep existing node labels expected by code.
4. Add TODO placeholders for unspecified pins instead of guessing.
5. Build and report.
