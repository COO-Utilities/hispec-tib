# AGENTS.md - HISPEC-FIB AI and Developer Rules

These rules are the canonical AI-assisted work rules for the HISPEC-FIB / HiSPEC-TIB Zephyr firmware repository. The workspace root `AGENTS.md` points here; `AGENTSv2.md` is a merged draft and must not be treated as a separate source of truth.

The goal is simple, explicit, maintainable C firmware that uses Zephyr correctly, avoids unnecessary code growth, and remains readable by non-expert embedded developers.

---

## 1. Operating Mode

Before acting, classify the request.

- Question, review, plan, or investigation: do not edit files unless the user explicitly asks for edits.
- Execute, fix, or implement: edit only the files needed for the stated goal.
- Debugging or uncertain root cause: prefer observations, hypotheses, and reversible probes over permanent code.
- Documentation reconciliation: report the mismatch and consult the user on the intended reconciliation before changing behavior or source-of-truth docs.

When the user is actively deciding what to keep, stop at findings and options. Do not clean up into a larger design unless requested.

Completed implementation work should end in coherent, reviewable commits unless
the user explicitly asks not to commit. If the next step requires human review
or an intent decision, pause with the smallest useful diff and ask for review
instead of continuing into speculative changes.

---

## 2. Maintenance-Era Simplification Posture

The initial buildout is complete enough that simplification is now a primary goal.

Prefer, in order:

1. Deleting code.
2. Reusing existing project code.
3. Using Zephyr-native APIs.
4. Adding small local code.
5. Adding one narrow cross-module API.
6. Adding a new file.

Before any substantial patch, ask:

- Can this be solved by removing or consolidating code?
- Is this already handled in `coo_commons`, Zephyr, or an existing module?
- Am I adding defensive code for impossible states in fixed static arrays?
- Is this debug instrumentation temporary? If yes, how will it be removed?
- Is this growing the command layer, hardware layer, public API, persistence model, or scheduling model unnecessarily?

Pause and explain why growth is justified before introducing any of these:

- a diff over roughly 100 lines,
- a new source or header file,
- a new public API,
- new persistent state,
- a new thread, workqueue, or scheduler-like mechanism,
- a helper used only once when inline code would be clear,
- duplicate command keys, aliases, or multiple ways to express the same operation.

New source/header files require explicit justification and should be avoided unless they remove more complexity than they add.

Do not roll custom drivers or register-level hardware access when Zephyr has an appropriate driver or API unless the user explicitly approves the exception.

---

## 3. Project Context

This is Zephyr RTOS firmware for the HISPEC FIB PCB controllers.

Primary implementation language is C, not C++. Avoid introducing C++ abstractions unless explicitly requested.

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
- clear code for developers who may not be Zephyr experts.

---

## 4. Source of Truth and Documentation Precedence

Read relevant documentation before editing.

Important project documents include:

- `hispec-tib/doc/architecture.md`
- `hispec-tib/doc/commands.md`
- `hispec-tib/doc/hardware.md`
- repository README files
- existing source comments and Doxygen comments

Source-of-truth order:

1. Developer-maintained docs and AI-agent rules govern project intent.
2. `commands.md` governs command/API behavior unless the task is explicitly to revise it.
3. `hardware.md` governs hardware mappings, pins, buses, addresses, GPIO polarities, physical assumptions, and board profiles.
4. Code governs current implementation state, but not necessarily intended final behavior.

If code and docs disagree:

1. Report the mismatch.
2. Identify affected files.
3. Propose concrete reconciliation options.
4. Ask the user which reconciliation is intended.
5. Do not silently choose a side.

Do not edit `hardware.md` unless explicitly asked or unless a concrete mismatch is identified first and the task includes documentation reconciliation.

Any edit to `hispec-tib/app/boards/nucleo_h563zi.overlay` must include a sync check against `hardware.md` in the same response.

---

## 5. Repository, Build Context, and Debug

Project git root is `hispec-tib`:

```bash
git -C ./hispec-tib status --short
```

The project uses the Python virtual environment at:

```bash
./.venv/
```

Use this venv for Python, west commands, and doc commands such as `sphinx-build`.

For Nucleo build validation, use:

```bash
./.venv/bin/west build --board=nucleo_h563zi/stm32h563xx --build-dir ./hispec-tib/app/build ./hispec-tib/app
```

Build discipline:

- Do not run parallel builds or build steps that share the same build directory.
- If running both `--cmake-only` and full build checks, run them sequentially.
- After DTS, overlay, driver, Kconfig, or build-config changes, run a real Nucleo build unless impossible.
- Be aware that west/CMake build directories can cache `EXTRA_CONF_FILE`; report when a build appears to include stale config.
- Report build pass/fail, the first blocking error if failed, newly introduced non-blocking warnings, and tests or static checks run.

Debug tooling may be in:

- `~/zephyr-sdk-1.0.1/`
- `/opt/ST/STM32CubeCLT_1.21.0`

Example:

```bash
~/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-addr2line -a -f -C -i -e hispec-tib/app/build/zephyr/zephyr.elf 0x0800f028 0x08010a95
```

Workspace metadata discipline:

- Do not edit files under `.idea/`. Those files are developer IDE state, not firmware source or documentation.

Search discipline:

- For broad source/documentation searches, exclude build directories by default.
- Search generated build outputs only when the build artifact itself is relevant,
  and then target the specific build directory, generated file, symbol, or DTS
  node being investigated.

---

## 6. Coding Style and Architecture

Keep it simple.

Follow Zephyr 4.x design patterns where appropriate.

Use high-level Zephyr functionality unless low-level APIs are necessary. For example, prefer Zephyr console facilities over low-level UART character handling for human serial console commands.

Avoid dynamic allocation. Prefer:

- fixed buffers,
- compile-time constants,
- static tables,
- Zephyr queues,
- Zephyr work items,
- clear ownership of shared state.

The firmware should remain explicit and static.

Avoid framework drift:

- no broad scheduler,
- no generic state-machine framework,
- no dynamic command registry,
- no runtime plugin pattern,
- no unnecessary manager/service/common/generic abstraction.

Small named tables and explicit switch/lookup functions are usually preferred.

Structure modularly and support graceful degradation only within the board/profile/hardware-presence policy below.

---

## 7. Ownership Boundaries

Before coding, identify which existing layer owns the behavior.

Do not start by adding a new abstraction.

Ownership guidance shall be pulled from documents described in Section 4.

Keep command handlers thin but not empty. They may parse constrained MQTT/serial payloads and shape responses.

Hardware sequencing and reusable domain decisions should live in the relevant domain module.

Do not move command-schema-specific parsing into hardware modules.

---

## 8. Helper and API Scope Rules

Prefer code inline when logic is local, short, and used once.

Add a helper only when it does at least one of these:

- Names a non-obvious hardware or protocol operation.
- Centralizes a safety check or side effect.
- Prevents duplicate parsing or validation with the same behavior.
- Encapsulates a Zephyr API call whose flags, blocking behavior, or return contract matter.
- Separates command parsing from domain action.

Do not hide a simple local policy behind a one-line wrapper when an inline call
plus a short "why" comment would be clearer. If a helper's main value is naming
policy rather than reducing real code duplication, its definition and call sites
must make that policy obvious to a fresh reader.

Helpers should start `static` and file-local.

Export a function only when another module has a real ownership-appropriate need for it.

Do not widen headers just to avoid deciding where code belongs.

If a helper's name needs `generic`, `common`, `manager`, or `service` but has only one caller, keep it local.

For unresolved symbol errors, check C linkage visibility first.

Preferred fix order:

1. Minimal symbol visibility fix for the exact function needed.
2. If needed, add one narrowly scoped exported wrapper.
3. Avoid large header refactors unless requested.

Do not broaden APIs unnecessarily.

---

## 9. Command Interface Discipline

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
- report errors precisely.

Commands should do as much safe work as possible. If partial failure occurs, report what succeeded, what failed, and what remains unknown.

Avoid multiple ways to do the same command operation. Do not add compatibility aliases, duplicate payload keys, parallel schemas, migrations, or backward-compatibility behavior unless the user explicitly requests it for that exact change.

When replacing a command key or response field, prefer one canonical schema and update firmware, `commands.md`, and Python helpers together. If existing behavior and docs disagree, consult the user before choosing the reconciliation.

Do not change command behavior without updating `commands.md` or explicitly flagging the documentation mismatch.

---

## 10. Hardware and Devicetree Discipline

`hardware.md` is the hardware source of truth.

Do not infer pins, buses, addresses, polarities, or physical mappings from code if docs disagree.

If code and docs disagree:

1. Report the mismatch.
2. Identify affected files.
3. Propose reconciliation options.
4. Ask the user which option reflects intended hardware truth.
5. Do not silently choose a side.

Devicetree / overlay rules:

- Preserve required node labels used by code.
- If adding hardware nodes, verify correct tree scope.
- If board-type strap pins are unspecified in docs, leave explicit TODO placeholders rather than inventing pins.
- Keep comments short and factual.
- No speculative behavior claims.

Do not use Zephyr POSIX support unless explicitly requested.

---

## 11. Kconfig and Hardware Presence Policy

Assume hardware existence according to selected board/module configuration.

One binary may be used for all boards, but a board-specific jumper/strap determines which hardware should configure and execute.

Only add runtime hardware-absent fallbacks as required by Zephyr or existing project policy.

Error handling should target transient runtime faults, such as bus and I/O failures, not absent-intent hardware unless the selected board profile indicates the hardware should exist.

A failed laser driver, disconnected ribbon cable, or failed peripheral should produce a clear local error and should not cause a broader system fault unless that escalation is explicitly part of the selected profile or safety policy.

Do not add active checking solely to prove low-probability hardware presence or absence. If a configured device faults during use, report the fault clearly and let other subsystems continue when safe.

---

## 12. Settings, State, Persistence, Warnings, and Telemetry

Do not duplicate state unless there is a clear restart, replacement, persistence, or ownership reason.

If hardware, a driver module, or EEPROM already owns a setting, app code should not mirror it just for convenience.

Persist app-level values such as:

- calibration,
- user intent,
- settings required to restore behavior after app reboot,
- settings required after hardware replacement.

Settings reads in timing-sensitive loops should be non-blocking or cached unless blocking is explicitly acceptable.

Preserve the non-blocking embedded-system posture.

Timing-sensitive code must not publish MQTT directly.

Prefer:

- bounded queues,
- best-effort warnings,
- explicit command responses,
- local logging fallback,
- dropped non-critical telemetry over blocking hardware timing paths.

Warnings are best-effort. Dropping a non-critical warning or telemetry message is acceptable. Blocking a timing-sensitive path is not.

The system operates best-effort. Operators can retry, issue a different command sequence, interrogate status, or intervene manually. Visibility and predictability win over complex brittle attempts to handle every edge case.

---

## 13. Timing, Logging, and Debug Instrumentation

Timing-sensitive code must not publish MQTT or block on nonessential telemetry.

Debug instrumentation must be:

- aggregated or rate-limited,
- easy to disable,
- clearly marked as diagnostic,
- removable in one obvious patch.

Before adding local timing probes, consider Zephyr tools first:

- thread analyzer,
- CPU load,
- tracing,
- GDB/ST-Link,
- stack/runtime stats.

For timing investigations, prefer reusable system-level observability over bespoke per-module bloat.

---

## 14. Zephyr Threads, Workqueues, and Scheduled Actions

Use Zephyr APIs and defaults unless there is measured evidence to change them.

Do not move work onto the system workqueue casually. App work that can block on hardware I/O should use an app-owned thread/workqueue or remain in its existing owner.

Do not change thread priorities or Kconfig scheduler behavior without stating:

- current priority/default,
- desired ordering,
- affected Zephyr/system threads,
- expected failure mode,
- test plan.

Scheduled actions are for small named firmware-delayed actions only, such as serial guard expiration, delayed reboot, auto-off timeout, or safety timeout.

Do not create a broad scheduler or user-programmable automation subsystem.

Each new scheduled action should be a named enum entry or equivalent concrete firmware behavior.

---

## 15. Documentation Requirements

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

Inline comments that explain why code is shaped a particular way are encouraged.
Prefer a local "why" comment over an extra helper when the code itself is simple
but the policy, hardware constraint, timing constraint, or Zephyr behavior is
easy to forget or misread.

Do not write comments that merely narrate C syntax.

Assume the audience is familiar with Arduino-style embedded work but not expert, is not expert in Zephyr development, may be primarily a Python coder, and may return to the codebase after a long absence.

---

## 16. TODO Handling

Do not remove TODOs tagged with `-jib`, `-JIB`, `-Jeb`, or similar without explicit instruction unless they are demonstrably resolved or obviated by removal of the relevant functional/namespace scope.

When cleaning documentation:

- remove addressed TODOs only if clearly resolved,
- move resolved-but-needs-human-review items into a dedicated "LLM resolved; human review requested" section,
- preserve intent-level TODOs that remain open,
- avoid erasing developer context.

Do not rename headings in `human_review_required.md`.

---

## 17. Maintenance and Repair Discipline

When fixing bugs, cleaning up rough code, or correcting earlier generated changes, prefer the smallest ownership-preserving edit:

1. Put code in the module that owns the behavior.
2. Make helper scope no wider than needed.
3. Remove duplication only when the duplicate behavior is truly the same.
4. Avoid broad refactors mixed with feature changes.
5. Preserve user TODOs unless the exact scope is resolved.
6. Update docs when behavior changes.

For simplification planning, identify concrete delete/consolidate targets, ownership boundaries, expected behavior impact, and verification before editing.

---

## 18. Response Style for This Repository

Use this response shape when practical:

1. Findings first.
2. Proposed edits or plan.
3. Verification performed.
4. Remaining risks or human-review items.

Use file and line references for concrete claims where available.

Keep recommendations concrete and patchable.

Call out uncertainty explicitly.

Do not overstate what was verified.

When code was not changed, say so. When tests/builds were not run, say so.

---

## 19. Embedded Maintenance Self-Review

Before finalizing, self-review:

- Did this introduce hidden state?
- Did this put hardware behavior in command glue?
- Did this expose a function that could have stayed static?
- Did this add a helper that obscures a simple local operation?
- Did this add duplicate command spelling, schema, or behavior?
- Did this document side effects sufficiently?
- Did this change command behavior without updating `commands.md`?
- Did this conflict with `hardware.md`?
- Did this add blocking behavior to a timing-sensitive path?
- Did this preserve best-effort warnings and telemetry?
- Did this require a west build?
- Were build steps sequential?
