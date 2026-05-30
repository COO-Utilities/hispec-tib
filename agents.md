# AGENTS.md — HiSPEC-TIB AI and Developer Rules

These rules apply to AI-assisted edits and reviews in the HiSPEC-TIB Zephyr firmware repository.

The goal is to keep the firmware simple, explicit, readable, and maintainable by non-expert embedded developers while still using Zephyr correctly.

---

## 1. Project Context

This is Zephyr RTOS firmware for the HISPEC FIB PCB controllers.

Primary implementation language is **C**, not C++. Avoid introducing C++ abstractions unless explicitly requested.

The firmware controls combinations of:

- MEMS optical switches.
- Variable optical attenuators.
- Photodiode monitoring ADCs.
- Maiman laser diode bank over Modbus.
- Board and relay power GPIOs.
- Networking, MQTT, SNTP, watchdog, settings, and serial console control.

The firmware should emphasize:

- static memory layout,
- bounded queues,
- explicit task/thread ownership,
- Zephyr-native APIs,
- graceful runtime error reporting,
- and clear code for developers who may not be Zephyr experts.

---

## 2. Source of Truth and Documentation Precedence

Read relevant documentation before editing.

Important project documents include:

- `hispec-tib/app/doc/status.md` or migrated equivalent under `hispec-tib/doc/`
- `hispec-tib/app/doc/commands.md` or migrated equivalent under `hispec-tib/doc/`
- `hispec-tib/app/doc/hardware.md` or migrated equivalent under `hispec-tib/doc/`
- `hispec-tib/app/doc/libraries.md` or migrated equivalent under `hispec-tib/doc/`
- `hispec-tib/app/doc/runtime_architecture.md` or migrated equivalent under `hispec-tib/doc/`
- `hispec-tib/doc/status.md`
- `hispec-tib/doc/nuisances.md`
- `hispec-tib/doc/breadboard.md`
- repository README files
- existing source comments and Doxygen comments

Source-of-truth order:

1. Developer-maintained docs and AI-agent rules govern project intent.
2. `commands.md` governs command/API behavior unless the task is explicitly to revise it.
3. `hardware.md` governs hardware mappings, pins, buses, addresses, GPIO polarities, physical assumptions, and board profiles.
4. Code governs current implementation state, but not necessarily intended final behavior.
5. If code and docs disagree, report the mismatch and propose a reconciliation plan before choosing a side.

Do not silently resolve intent-level conflicts.

Do NOT silently resolve intent-level conflicts.

Do not edit `hardware.md` unless explicitly asked or unless a concrete mismatch is identified first and the task includes documentation reconciliation.

Any edit to `hispec-tib/app/boards/nucleo_h563zi.overlay` must include a sync check against `hardware.md` in the same response.

---

## 3. Repository and Build Context

Project git root is:


```bash
bash git -C ./hispec-tib status --short
```

The project uses the Python virtual environment at:

```bash
bash ./.venv/
```

Use this venv for Python and west commands.

For Nucleo build validation, use:

```bash
bash ./.venv/bin/west build
--board=nucleo_h563zi/stm32h563xx
--build-dir ./hispec-tib/app/build
./hispec-tib/app
```


Build discipline:

- Do not run parallel builds or build steps that share the same build directory.
- If running both `--cmake-only` and full build checks, run them sequentially.
- After DTS, overlay, driver, or Kconfig changes, run a real Nucleo build unless impossible.
- Report:
    - build pass/fail,
    - first blocking error if failed,
    - newly introduced non-blocking warnings,
    - tests or static checks run.

Workspace metadata discipline:

- Do not edit files under `.idea/`. Those files are developer IDE state, not
  firmware source or documentation.

---

## 4. Coding Style and Architecture

Keep it simple.

Follow Zephyr 4.x design patterns where appropriate.

Use high-level Zephyr functionality unless low-level APIs are necessary. For example, prefer Zephyr console facilities over low-level UART character handling for human serial console commands.

Avoid dynamic allocation. Prefer:

- fixed buffers,
- compile-time constants,
- static tables,
- Zephyr queues,
- Zephyr work items,
- and clear ownership of shared state.

The firmware should remain explicit and static.

Avoid framework drift:

- no broad scheduler,
- no generic state-machine framework,
- no dynamic command registry,
- no runtime plugin pattern,
- no unnecessary “manager/service/common/generic” abstraction.

Small named tables and explicit switch/lookup functions are usually preferred.

Structure modularly and support graceful degradation where appropriate, but only within the board/profile/hardware-presence policy below.

---

## 5. Ownership Boundaries

Before coding, identify which existing layer owns the behavior.

Do not start by adding a new abstraction.

Ownership guidance shall be pulled from documents described in Section 2.

Keep command handlers thin but not empty. They may parse constrained MQTT/serial payloads and shape responses. 
Hardware sequencing and reusable domain decisions should live in the relevant domain module.

Do not move command-schema-specific parsing into hardware modules.

---

## 6. Helper and API Scope Rules

Prefer code inline when logic is local, short, and used once.

Add a helper when it does at least one of these:

- Names a non-obvious hardware or protocol operation.
- Centralizes a safety check or side effect.
- Prevents duplicate parsing/validation with the same behavior.
- Encapsulates a Zephyr API call whose flags, blocking behavior, or return contract matter.
- Separates command parsing from domain action.

Helpers should start `static` and file-local.

Export a function only when another module has a real ownership-appropriate need for it.

Do not widen headers just to avoid deciding where code belongs.

If a helper’s name needs `generic`, `common`, `manager`, or `service` but has only one caller, keep it local.

For unresolved symbol errors, check C linkage visibility first.

Preferred fix order:

1. Minimal symbol visibility fix for the exact function needed.
2. If needed, add one narrowly scoped exported wrapper.
3. Avoid large header refactors unless requested.

Do not broaden APIs unnecessarily.

---

## 7. Command Interface Discipline

`commands.md` is the authoritative command/API specification unless the task is explicitly to revise it.

Serial and MQTT should share the same normalized payload behavior where practical.

Assume parsing is constrained and brittle. Validate:

- topic/key lengths,
- known names,
- enum values,
- numeric ranges,
- required fields,
- unsupported fields when meaningful,
- payload buffer limits.

All commands and replies must account for embedded string parsing limitations and MQTT payload constraints.

Command handlers should:

- parse command-specific payloads,
- validate inputs,
- shape responses,
- call domain modules for hardware behavior,
- avoid hardware timing loops,
- and report errors precisely.

Commands should do as much safe work as possible. If partial failure occurs, report what succeeded, what failed, and what remains unknown.

Do not change command behavior without updating `commands.md` or explicitly flagging the documentation mismatch.

---

## 8. Hardware and Devicetree Discipline

`hardware.md` is the hardware source of truth.

Do not infer pins, buses, addresses, polarities, or physical mappings from code if docs disagree.

If code and docs disagree:

1. Report the mismatch.
2. Identify affected files.
3. Propose reconciliation.
4. Do not silently choose a side unless explicitly instructed.

Devicetree / overlay rules:

- Preserve required node labels used by code.
- If adding hardware nodes, verify correct tree scope.
- If board-type strap pins are unspecified in docs, leave explicit TODO placeholders rather than inventing pins.
- Keep comments short and factual.
- No speculative behavior claims.

Do not use Zephyr POSIX support unless explicitly requested.

---

## 9. Kconfig and Hardware Presence Policy

Assume hardware existence according to selected board/module configuration.

One binary may be used for all boards, but a board-specific jumper/strap determines which hardware should configure and execute.

Only add runtime “hardware absent” fallbacks as required by Zephyr or existing project policy.

Error handling should target transient runtime faults, such as bus and I/O failures, not absent-intent hardware unless 
the selected board profile indicates the hardware should exist.

---

## 10. Settings, State, Persistence, Warnings, and Telemetry

Do not duplicate state unless there is a clear restart, replacement, or persistence reason.

If hardware, a driver module, or EEPROM already owns a setting, app code should not mirror it just for convenience.

Persist app-level values such as:

- calibration,
- user intent,
- settings required to restore behavior after app reboot,
- settings required after hardware replacement.

Preserve the non-blocking embedded-system posture.

Timing-sensitive code must not publish MQTT directly.

Prefer:

- bounded queues,
- best-effort warnings,
- explicit command responses,
- local logging fallback,
- and dropped non-critical telemetry over blocking hardware timing paths.

Warnings are best-effort. Dropping a non-critical warning or telemetry message is acceptable. Blocking a timing-sensitive path is not.

The system operates best-effort. Operators can retry, issue a different command sequence, interrogate status, or 
intervene manually. Visibility and predictability win over complex brittle attempts to handle every edge case.

---

## 11. Scheduled Actions

`app_scheduled_actions.c` owns small named firmware-delayed actions only.

Use it for cases such as:

- serial guard expiration,
- delayed reboot,
- auto-off timeout,
- safety timeout.

Do not create a broad scheduler or user-programmable automation subsystem.

Each new scheduled action should be a named enum entry with a concrete firmware behavior.

---

## 12. Documentation Requirements

Document functions introduced or materially changed for intent and side effects.

Documentation should explain:

- what the function is for,
- what hardware/protocol state it may change,
- whether it can sleep,
- whether it can enqueue,
- whether it can publish,
- whether it can block on I/O,
- what owns persistence or calibration state,
- what values are source-of-truth versus estimates/defaults.

Place documentation near the code that needs it.

Briefly document Zephyr API calls near the line when flags or behavior are easy to misread, especially:

- GPIO logical polarity,
- `gpio_pin_configure_dt()` and `gpio_pin_get_dt()`,
- `k_work_delayable`,
- `k_msgq`,
- Modbus transactions,
- settings callbacks,
- blocking sleeps,
- MQTT properties,
- ADC conversion assumptions,
- devicetree-derived behavior.

Keep comments short and factual.

Do not write comments that merely narrate C syntax.

Assume the audience is familiar with Arduino-style embedded work but not expert, is not expert in Zephyr development, 
may be primarily a python coder, and may return to the codebase after a long absence.

---

## 13. TODO Handling

Do not remove TODOs tagged with `-jib`, `-JIB`, `-Jeb`, or similar without explicit instruction unless they are 
demonstrably resolved or obviated by removal of the relevant functional/namespace scope.

When cleaning documentation:

- remove addressed TODOs only if clearly resolved,
- move resolved-but-needs-human-review items into a dedicated “LLM resolved; human review requested” section,
- preserve intent-level TODOs that remain open,
- avoid erasing developer context.

---

## Maintenance and Repair Discipline

When fixing bugs, cleaning up rough code, or correcting earlier generated changes, prefer the smallest ownership-preserving edit:

1. Put code in the module that owns the behavior.
2. Make helper scope no wider than needed.
3. Remove duplication only when the duplicate behavior is truly the same.
4. Avoid broad refactors mixed with feature changes.
5. Preserve user TODOs unless the exact scope is resolved.
6. Update docs when behavior changes.

---

## 15. Response Style for This Repository

Use this response shape when practical:

1. Findings first.
2. Proposed edits or plan.
3. Verification performed.
4. Remaining risks or human-review items.

Use file and line references for concrete claims where available.

Keep recommendations concrete and patchable.

Call out uncertainty explicitly.

Do not overstate what was verified.

---

## 16. Embedded Maintenance Self-Review

Before finalizing, self-review:

- Did this introduce hidden state?
- Did this put hardware behavior in command glue?
- Did this expose a function that could have stayed static?
- Did this add a helper that obscures a simple local operation?
- Did this document side effects sufficiently?
- Did this change command behavior without updating `commands.md`?
- Did this conflict with `hardware.md`?
- Did this add blocking behavior to a timing-sensitive path?
- Did this preserve best-effort warnings and telemetry?
- Did this require a west build?
- Were build steps sequential?
