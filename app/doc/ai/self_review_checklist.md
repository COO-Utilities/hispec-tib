# AI Self-Review Checklist (HiSPEC-TIB)

Use this before accepting any AI-generated patch.

## A) Doc/Code Sync
- [ ] `hardware.md` and Nucleo overlay agree on bus, address, pin mapping.
- [ ] Overlay comments reflect actual nodes.

## B) Devicetree Correctness
- [ ] Required node labels still exist.
- [ ] New nodes are under correct parent scope.
- [ ] `zephyr,user` properties used by code are present.

## C) Linkage/API Correctness
- [ ] No unnecessary API widening.
- [ ] No dead wrappers or duplicate symbols.

## D) Behavior Contract
- [ ] No runtime fallback was added for compile-time-selected hardware intent.
- [ ] Error handling targets runtime IO faults, not absence of intended hardware.

## E) Build Proof
- [ ] If `west` is not in PATH, retry with `/Users/jibailey/zephyrproject/.venv/bin/west` before reporting failure.
- [ ] Nucleo west build run with explicit board/build-dir/app path.
- [ ] First blocker captured if failed; no blocker if passed.
- [ ] New warnings explained if introduced.

## F) Output Quality
- [ ] Findings listed first with file:line refs.
- [ ] Patch scope is minimal and aligned to request.
