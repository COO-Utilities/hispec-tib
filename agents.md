# AGENTS.md — HISPEC-FIB / HiSPEC-TIB AI and Developer Rules

These are the canonical AI-assisted development rules for the HISPEC-FIB / HiSPEC-TIB Zephyr firmware repository.

The primary goal is simple, explicit, maintainable C firmware that uses Zephyr correctly, remains readable after long gaps, and does not accumulate generated architecture. The repository is not a place for agent-designed frameworks, compatibility layers, speculative abstractions, or production-style migration machinery unless the human owner explicitly asks for them.

This code is currently desk/lab firmware unless the human owner explicitly says otherwise. Backward compatibility is not assumed for unreleased firmware commands, host-tool APIs, Python parsers, telemetry schemas, settings fields, debug formats, or calibration data formats.

---

## 1. Non-Negotiable Operating Rules

Before acting, classify the request and state the classification.

Allowed classifications:

1. **Question / explanation**

  * Do not edit files.
  * Inspect the relevant implementation before claiming behavior.

2. **Investigation / review**

  * Do not edit files.
  * Produce findings, source references, uncertainty, and concrete options.

3. **Plan**

  * Do not edit files.
  * Produce a bounded patch plan with delete/consolidate opportunities first.

4. **Implement**

  * Edit only files required for the approved scope.
  * Prefer the smallest reviewable diff.
  * End with verification and a precise change summary.

5. **Simplify / compact / refactor**

  * This is deletion-first work.
  * Do not add abstractions unless they remove more code and make ownership clearer.
  * Do not mix behavior changes with compaction unless explicitly approved.

6. **Debugging / lab probe**

  * Prefer raw observability, reversible probes, and narrow instrumentation.
  * Do not build permanent infrastructure around an unvalidated physical model.

When the human is deciding what to keep, stop at findings and options. Do not “clean up” into a larger design unless explicitly requested.

If a request is ambiguous between review and implementation, choose review.

---

## 2. Mandatory Project Exploration Before Code Changes

For any implementation, simplification, or bug fix, first build a local picture of the relevant code.

Before editing, inspect:

* the directly named file(s),
* direct callers and callees,
* relevant headers,
* relevant command handlers,
* relevant docs under `doc/`,
* relevant Python host-tool support code if command/API behavior changes,
* relevant settings/persistence code if values are stored or restored,
* relevant TODOs in code and `doc/human_review_required.md`.

The response before a substantial patch must include a short scout report:

```text
Scope classification:
Files inspected:
Primary owner module:
Call path:
State/persistence touched:
Command/API touched:
Docs touched:
Deletion/consolidation opportunities:
Uncertainties / owner decisions needed:
```

Do not infer behavior from names, comments, or previous sessions when the implementation is available. Read the implementation.

If the implementation contradicts docs or TODOs, report the mismatch and ask which source should be reconciled unless the task explicitly says which behavior is intended.

---

## 3. Simplification Is the Default

The repository already has enough functionality that simplification is a primary goal.

Prefer, in order:

1. Delete code.
2. Inline simple one-use helpers.
3. Reuse existing local code.
4. Use Zephyr-native APIs.
5. Move code to its true owner.
6. Add a small local helper.
7. Add one narrow cross-module API.
8. Add a new file only when it removes more complexity than it adds.

Before any substantial patch, answer:

* Can this be solved by deleting code?
* Can this be solved by making an existing owner do its job?
* Can this be solved by replacing a compatibility path with one canonical path?
* Is this defensive code for an impossible state in fixed static firmware?
* Is this debug instrumentation temporary?
* Is this preserving an unreleased interface unnecessarily?
* Is this moving behavior into command glue that belongs in a domain module?
* Is this adding a helper that hides a simple operation?

A simplification patch must report:

```text
Lines added:
Lines deleted:
Net line change:
Functions added:
Functions deleted:
Files added:
Files deleted:
Public APIs added/removed:
Compatibility behavior removed:
Behavior intentionally unchanged:
```

A simplification patch that increases net code size requires explicit justification and should normally be rejected.

---

## 4. Hard Limits on Bloat

Pause and ask for approval before introducing any of the following:

* diff over roughly 100 net-added lines,
* new source file,
* new header file,
* new public API,
* new command key,
* new response schema,
* new persistent setting,
* new compatibility alias,
* new telemetry format,
* new thread,
* new workqueue,
* new scheduler-like mechanism,
* new manager/service/framework abstraction,
* new generic registry,
* new state-machine framework,
* new dynamically extensible pattern,
* helper used only once unless it names a non-obvious hardware/protocol operation.

Do not compensate for uncertainty by adding machinery.

When unsure, produce a short investigation patch or no patch.

---

## 5. Unreleased-Code Rule: No Backward Compatibility Unless Requested

Assume this firmware and Python tooling are unreleased personal desk/lab code.

Therefore:

* Do not preserve old command spellings.
* Do not accept old JSON keys.
* Do not keep old Python parsers.
* Do not support old telemetry formats.
* Do not maintain old binary payload layouts.
* Do not add migration code for old settings unless explicitly requested.
* Do not support multiple names for one operation.
* Do not support settings persistence or migration.

When replacing an unreleased interface, delete the old interface and update the firmware, docs, and Python host helper together.

One canonical behavior is preferred. "Compatibility" creates confusion.

---

## 6. Human Review Discipline

`doc/human_review_required.md` is the owner-review ledger.

Do not rename headings in that file.

Do not move items into “LLM Resolved; Human Review Requested” unless:

* the code was changed,
* docs or Python helpers were updated when relevant,
* verification was run or explicitly impossible,
* the remaining human action is truly validation rather than code review of agent uncertainty.

Do not create human-review debt to hide incomplete work.

For each affected item, do one of:

* leave it untouched,
* mark it resolved only when demonstrably resolved,
* add a concise note explaining exactly what remains for human validation,
* split a mixed item into smaller owner-review items if needed.

Do not erase owner TODOs tagged with `-jib`, `-JIB`, `-Jeb`, or similar unless the exact scope is resolved or the relevant code is deleted.

---

## 7. TODO Handling

TODOs are not decoration. They are owner signals.

When touching a file with TODOs:

1. Read nearby code.
2. Decide whether each nearby TODO is:

  * resolved by the current patch,
  * still valid,
  * contradicted by current code,
  * an architecture decision requiring owner input,
  * obsolete because the code should be deleted.
3. Do not silently remove or rewrite TODOs.
4. Do not implement TODOs opportunistically outside the approved scope.

If a TODO says code belongs in another module, do not immediately move it. First inspect ownership and propose a minimal reconciliation.

For TODOs in `main.c`, be especially careful: many are architecture/ownership questions around MQTT, command dispatch, logging, workqueues, thread priorities, and boot sequencing. Treat them as consolidation targets, not invitations to add more orchestration.

---

## 8. Project Context

This is Zephyr RTOS firmware for HISPEC FIB PCB controllers.

Primary implementation language is C, not C++. Avoid introducing C++ abstractions unless explicitly requested.

The firmware controls combinations of:

* MEMS optical switches,
* variable optical attenuators,
* photodiode monitoring ADCs,
* Maiman laser diode bank over Modbus,
* board and relay power GPIOs,
* networking,
* MQTT,
* SNTP,
* watchdog,
* settings,
* serial console control.

The firmware should emphasize:

* static memory layout,
* bounded queues,
* explicit task/thread ownership,
* Zephyr-native APIs,
* graceful runtime error reporting,
* clear code for developers who may not be Zephyr experts.

Avoid dynamic allocation.

Prefer:

* fixed buffers,
* compile-time constants,
* static tables,
* Zephyr queues,
* Zephyr work items,
* explicit ownership of shared state.

---

## 9. Source of Truth and Documentation Precedence

Read relevant documentation before editing.

Important project documents include:

* `hispec-tib/doc/architecture.md`
* `hispec-tib/doc/commands.md`
* `hispec-tib/doc/hardware.md`
* `hispec-tib/doc/human_review_required.md`
* repository README files
* existing source comments and Doxygen comments

Source-of-truth order:

1. Human owner instruction in the current task.
2. Developer-maintained docs and this `AGENTS.md`.
3. `commands.md` for command/API behavior.
4. `hardware.md` for hardware mappings, pins, buses, addresses, GPIO polarities, physical assumptions, and board profiles.
5. Code for current implementation state.

Code describes what exists, not necessarily what is intended.

If code and docs disagree:

1. Report the mismatch.
2. Identify affected files.
3. Propose concrete reconciliation options.
4. Ask which reconciliation is intended.
5. Do not silently choose a side.

Do not edit `hardware.md` unless explicitly asked or unless a concrete mismatch is identified and the task includes documentation reconciliation.

Any edit to `hispec-tib/app/boards/nucleo_h563zi.overlay` must include a sync check against `hardware.md`.

---

## 10. Repository, Build Context, and Search Discipline

Project git root is `hispec-tib`:

```bash
git -C ./hispec-tib status --short
```

Use the Python virtual environment at:

```bash
./.venv/
```

Use this venv for Python, west commands, and documentation commands.

For Nucleo build validation, use:

```bash
./.venv/bin/west build --board=nucleo_h563zi/stm32h563xx --build-dir ./hispec-tib/app/build ./hispec-tib/app
```

Build discipline:

* Do not run parallel builds sharing one build directory.
* If running `--cmake-only` and a full build, run sequentially.
* After DTS, overlay, driver, Kconfig, or build-config changes, run a real Nucleo build unless impossible.
* Report stale CMake or `EXTRA_CONF_FILE` cache suspicion.
* Report pass/fail, first blocking error, new warnings, and tests/static checks run.

Search discipline:

* Exclude build directories by default.
* Search generated build outputs only when the artifact itself is relevant.
* Prefer targeted `rg` searches over broad speculative browsing.
* When modifying a public function, search all call sites.
* When modifying a command schema, search firmware, docs, and Python helpers.

Do not edit files under `.idea/`.

---

## 11. Layer Ownership

Before coding, identify which existing layer owns the behavior.

Do not start by adding a new abstraction.

General ownership:

* Command handlers parse constrained MQTT/serial payloads, validate inputs, shape responses, and call domain modules.
* Domain modules own hardware sequencing and reusable domain policy.
* Hardware modules own device interaction, physical limits, and direct Zephyr driver calls.
* Settings modules own persistence format and restoration.
* Python host helpers own notebook ergonomics and lab plotting, not firmware policy.
* Documentation owns intended public behavior.

Do not move command-schema-specific parsing into hardware modules.

Do not put hardware timing loops in command glue.

Do not widen headers just to avoid deciding where code belongs.

---

## 12. Helper and API Scope Rules

Prefer inline code when logic is local, short, and used once.

Add a helper only when it does at least one of these:

* names a non-obvious hardware or protocol operation,
* centralizes a safety check or side effect,
* prevents real duplicate parsing or validation with identical behavior,
* encapsulates a Zephyr API call whose flags, blocking behavior, or return contract matter,
* separates command parsing from domain action.

Do not hide a simple local policy behind a one-line wrapper.

Helpers should start `static` and file-local.

Export a function only when another module has a real ownership-appropriate need.

For unresolved symbol errors, check linkage visibility first.

Preferred fix order:

1. Minimal symbol visibility fix for the exact function needed.
2. One narrowly scoped exported wrapper if needed.
3. No large header refactor unless requested.

Avoid names containing `generic`, `common`, `manager`, or `service` for one-caller helpers.

---

## 13. Command Interface Discipline

`commands.md` is authoritative for command/API behavior unless the task is explicitly to revise it.

Serial and MQTT should share normalized payload behavior where practical.

Validate:

* topic/key lengths,
* known names,
* enum values,
* numeric ranges,
* required fields,
* unsupported fields when meaningful,
* payload buffer limits.

All commands and replies must account for embedded string parsing limits and MQTT payload constraints.

Commands should do as much safe work as possible. If partial failure occurs, report what succeeded, what failed, and what remains unknown.

Avoid multiple ways to do the same operation.

When replacing a command key or response field:

* choose one canonical schema,
* delete unreleased old spellings,
* update firmware,
* update `commands.md`,
* update Python helpers.

Do not change command behavior without updating docs or explicitly flagging the mismatch.

---

## 14. Python Host Tool Discipline

Python tools are lab ergonomics, not a compatibility museum.

For unreleased firmware:

* delete old parsers when schemas change,
* delete old methods when command names change,
* do not maintain aliases,
* do not emulate old firmware behavior,
* prefer NumPy record arrays for lab datasets,
* keep plotting honest about what is measured versus inferred.

For calibration, model validation, and exploratory lab work:

* store raw records first,
* fit second,
* persist only after explicit acceptance,
* do not build large retrieval frameworks around unvalidated models,
* avoid JSON page machinery for dense numeric datasets,
* prefer a compact binary payload or clean serial dump when appropriate.

A Python plotting function should make assumptions visible. Do not label derived quantities as physical truth before calibration exists.

---

## 15. Hardware and Devicetree Discipline

`hardware.md` is the hardware source of truth.

Do not infer pins, buses, addresses, polarities, or physical mappings from code if docs disagree.

If board-type strap pins or profile rules are unspecified, leave explicit TODOs rather than inventing pins.

Devicetree / overlay rules:

* preserve required node labels used by code,
* verify correct tree scope when adding nodes,
* keep comments short and factual,
* no speculative behavior claims.

Do not use Zephyr POSIX support unless explicitly requested.

---

## 16. Kconfig and Hardware Presence Policy

Assume hardware existence according to selected board/module configuration.

One binary may be used for all boards, but a board-specific jumper/strap determines which hardware should configure and execute.

Only add runtime hardware-absent fallbacks as required by Zephyr or existing project policy.

A failed laser driver, disconnected ribbon cable, or failed peripheral should produce a clear local error and should not cause a broader system fault unless that escalation is explicitly part of the selected profile or safety policy.

Do not add active checking solely to prove low-probability hardware presence or absence.

If configured hardware faults during use, report the fault clearly and let other subsystems continue when safe.

Do not add null checks for static app-owned workqueues, devices, or queues merely to silence theoretical concerns. If a static required app object is not initialized, the app should fail clearly rather than obscure the programming error.

---

## 17. Settings, State, Persistence, Warnings, and Telemetry

Do not duplicate state unless there is a clear restart, replacement, persistence, or ownership reason.

If hardware, a driver, or EEPROM owns a setting, app code should not mirror it just for convenience.

Persist app-level values such as:

* calibration,
* user intent,
* settings required to restore behavior after app reboot,
* settings required after hardware replacement.

Settings reads in timing-sensitive loops should be non-blocking or cached unless blocking is explicitly acceptable.

Timing-sensitive code must not publish MQTT directly.

Prefer:

* bounded queues,
* best-effort warnings,
* explicit command responses,
* local logging fallback,
* dropped non-critical telemetry over blocking timing paths.

Warnings are best-effort. Dropping non-critical warning or telemetry is acceptable. Blocking a timing-sensitive path is not.

Console logs should be written assuming the console may be unmonitored. Important operator-relevant algorithmic status should have a deliberate telemetry path, not accidental `LOG_INF` dependence.

Do not replace every log call with a new logging framework unless explicitly approved.

---

## 18. Timing, Threads, Workqueues, and Scheduling

Use Zephyr APIs and defaults unless measured evidence supports changes.

Do not move work onto the system workqueue casually. App work that can block on hardware I/O should use an app-owned thread/workqueue or remain with its existing owner.

Do not change thread priorities without stating:

* current priority/default,
* proposed priority,
* desired ordering,
* affected Zephyr/system threads,
* expected failure mode,
* verification plan.

Thread priorities and responsibilities should be documented near their definitions or in architecture docs.

Scheduled actions are for small named firmware-delayed actions only, such as:

* serial guard expiration,
* delayed reboot,
* auto-off timeout,
* safety timeout.

Do not create a broad scheduler or user-programmable automation subsystem.

Each new scheduled action should be a named enum entry or equivalent concrete firmware behavior.

---

## 19. Debug Instrumentation and Lab Algorithms

Debug instrumentation must be:

* aggregated or rate-limited,
* easy to disable,
* clearly marked as diagnostic,
* removable in one obvious patch.

Before adding local timing probes, consider Zephyr tools first:

* thread analyzer,
* CPU load,
* tracing,
* GDB/ST-Link,
* stack/runtime stats.

For lab algorithms, especially calibration:

* preserve the owner’s algorithmic design notes near the implementation until the algorithm is validated,
* do not delete design rationale because code exists,
* collect raw data sufficient to diagnose the physical model,
* separate acquisition success from fit success,
* do not persist failed or partial fits unless explicitly requested,
* do not terminate acquisition merely because a fit would fail,
* do not represent saturated samples as high-confidence measurements,
* do not build extensive data-retrieval infrastructure before the physical model is validated.

A failed fit after complete acquisition should normally be represented as:

```text
acquisition = complete
fit = failed
persisted = false
data = available for debug
```

A true acquisition error is reserved for failures such as impossible routing, actuator write failure, all samples invalid in a required average, or an explicit operator stop.

---

## 20. Photodiode and ADC Error Policy

Photodiode monitoring owns ADC sample acquisition.

A transient ADC read/write failure during sampling should normally:

* discard that sample,
* emit a warning,
* continue the window,
* report fewer valid samples and larger/noisier uncertainty.

Averaging should fail only when the window has zero valid samples or when the photodiode subsystem itself cannot operate.

Callers should not retry individual ADC samples. A user-level command may be retried by the user.

If a caller receives an average with zero valid samples, that is a terminal average failure and should be reported clearly.

Do not tightly couple photodiode monitoring errors to unrelated algorithms unless every sample in the relevant window failed.

---

## 21. Documentation Requirements

Document functions introduced or materially changed for intent and side effects.

Documentation should explain:

* what the function is for,
* what hardware/protocol state it may change,
* whether it can sleep,
* whether it can enqueue,
* whether it can publish,
* whether it can block on I/O,
* what owns persistence or calibration state,
* what values are source-of-truth versus estimates/defaults.

Always place documentation near the code that needs it.

Briefly document Zephyr API calls when flags or behavior are easy to misread, especially:

* GPIO logical polarity,
* `gpio_pin_configure_dt()` and `gpio_pin_get_dt()`,
* `k_work_delayable`,
* `k_msgq`,
* Modbus transactions,
* settings callbacks,
* blocking sleeps,
* MQTT properties,
* ADC conversion assumptions,
* devicetree-derived behavior.

Keep comments short and factual.

Prefer a local “why” comment over an extra helper when the code itself is simple but the policy, hardware 
constraint, timing constraint, or Zephyr behavior is easy to forget.

### Explanatory Comment Creation and Preservation

Explanatory comments are vital, especially in algorithms, hardware behavior, calibration flows, physical models,
and command schemas.

Explanatory text outranks generic code-style preferences. A terse implementation is not better if it erases 
physical reasoning, lab context, failure-mode rationale, or intended operator interpretation.

Do not remove or shorten explanatory comments merely because the code appears self-explanatory, production-style,
or cleaner without them.

When a comment is stale:
1. preserve the underlying intent if still relevant,
2. rewrite it to match current behavior,
3. explicitly call out the rewrite in the response.

Only delete explanatory comments when:
- the described behavior no longer exists,
- the comment is actively misleading,
- and the lost rationale is either obsolete or replaced nearby.

For calibration, photodiode, MEMS routing, laser control, settings/persistence,
and command schema work, pre-commit/final summaries must state:
- explanatory comments removed: yes/no
- explanatory comments rewritten: yes/no
- why any removal was necessary

---

## 22. Maintenance and Repair Discipline

When fixing generated or previously overgrown code, prefer the smallest ownership-preserving edit:

1. Put code in the module that owns the behavior.
2. Make helper scope no wider than needed.
3. Remove duplication only when duplicate behavior is truly identical.
4. Avoid broad refactors mixed with feature changes.
5. Preserve owner TODOs unless the exact scope is resolved.
6. Update docs when behavior changes.
7. Update Python helpers when command/API behavior changes.

For simplification planning, identify:

* concrete delete targets,
* concrete consolidation targets,
* ownership boundaries,
* expected behavior impact,
* verification steps.

Do not “simplify” by creating a new framework.

---

## 23. Commit Discipline

Completed implementation work should end in coherent, reviewable commits unless the human explicitly asks not to commit.

A commit should represent one conceptual change.

Do not mix:

* behavior change and cleanup,
* formatting and logic,
* schema change and unrelated refactor,
* calibration algorithm change and plotting rewrite,
* docs reconciliation and hardware behavior change,

unless the human explicitly approves a combined patch.

Before committing, report:

```text
Commit intent:
Files changed:
Behavior changed:
Behavior intentionally unchanged:
Docs updated:
Python updated:
Build/tests run:
Known remaining issues:
```

Do not commit speculative work when the next step requires human intent.

---

## 24. Required Response Shape

Use this response shape when practical:

1. **Findings**
2. **Proposed edits / plan**
3. **Verification performed**
4. **Remaining risks / human-review items**

For implementation responses, include:

```text
Changed:
Deleted:
Added:
Net effect:
Verification:
Not verified:
```

Use file and line references for concrete claims when available.

Do not overstate what was verified.

When code was not changed, say so.

When tests/builds were not run, say so.

---

## 25. Embedded Maintenance Self-Review

Before finalizing, self-review:

* Did this introduce hidden state?
* Did this preserve unreleased compatibility without being asked?
* Did this add a schema alias?
* Did this put hardware behavior in command glue?
* Did this expose a function that could have stayed static?
* Did this add a helper that obscures a simple local operation?
* Did this add duplicate command spelling, schema, or behavior?
* Did this document side effects sufficiently?
* Did this change command behavior without updating `commands.md`?
* Did this conflict with `hardware.md`?
* Did this add blocking behavior to a timing-sensitive path?
* Did this preserve best-effort warnings and telemetry?
* Did this create human-review debt instead of resolving or preserving it honestly?
* Did this require a west build?
* Were build steps sequential?
* Did this make the code smaller, clearer, or better-owned?
* If not, why is the added complexity justified?

If the answers are not acceptable, stop and revise before final response.

---

## 26. Explicit Anti-Patterns

Do not do these unless explicitly requested:

* production migration code for unreleased schemas,
* compatibility aliases for unreleased commands,
* JSON page retrieval for dense numeric lab data,
* broad managers/services/frameworks,
* generic state-machine frameworks,
* dynamic command registries,
* runtime plugin patterns,
* speculative health-check infrastructure,
* null-check paranoia around required static app objects,
* wrapping every simple call in a helper,
* adding diagnostics that cannot be removed cleanly,
* preserving old Python APIs after firmware schema changes,
* replacing simple direct code with a larger abstraction and calling it “cleanup,”
* continuing implementation after discovering an ownership or intent ambiguity.

When an anti-pattern seems necessary, pause and ask.

---

## 27. Preferred Small-Patch Workflow

For a complex area, use this sequence:

1. **Scout**

  * read code/docs/callers,
  * report ownership,
  * identify delete/consolidate targets.

2. **Stabilize**

  * fix the smallest blocking bug,
  * add only necessary observability.

3. **Simplify**

  * delete obsolete code,
  * collapse duplicate paths,
  * remove compatibility.

4. **Reconcile**

  * update docs and Python helpers,
  * update human-review items.

5. **Verify**

  * build,
  * run targeted checks,
  * report limitations.

Do not jump from scout directly to architecture.

---

## 28. Project-Specific Build and Debug Notes

Useful command:

```bash
./.venv/bin/west build --board=nucleo_h563zi/stm32h563xx --build-dir ./hispec-tib/app/build ./hispec-tib/app
```

Debug tooling may be in:

```bash
~/zephyr-sdk-1.0.1/
/opt/ST/STM32CubeCLT_1.21.0
```

Example addr2line:

```bash
~/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-addr2line -a -f -C -i -e hispec-tib/app/build/zephyr/zephyr.elf 0x0800f028 0x08010a95
```

Always report exactly what was run.

---

## 29. Final Rule

The human owner must be able to return after weeks away, open the relevant file, and understand the control flow, ownership, and hardware assumptions without reconstructing an agent’s hidden design.

If a patch makes that harder, it is probably wrong even if it builds.
