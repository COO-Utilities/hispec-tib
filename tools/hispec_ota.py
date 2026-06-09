#!/usr/bin/env python3
"""Operator-driven firmware OTA helper for HISPEC FIB boards.

The script uses the firmware MQTT command path to open a temporary OTA window
and to reboot safely. Image upload/test remains delegated to a standard
MCUmgr-compatible CLI so this repository does not carry SMP protocol code.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Sequence

from hispec_fibpcb import DEVICE_NAMES, HispecFibError, HispecFibPcb


HASH_RE = re.compile(r"\b[0-9a-fA-F]{32,64}\b")


def _run(cmd: Sequence[str], *, dry_run: bool) -> subprocess.CompletedProcess[str] | None:
    print("+", " ".join(cmd))
    if dry_run:
        return None
    return subprocess.run(cmd, check=True, text=True, capture_output=True)


def _mcumgr_base(exe: str, host: str, port: int) -> list[str]:
    return [exe, "--conntype", "udp", "--connstring", f"{host}:{port}"]


def _print_completed(result: subprocess.CompletedProcess[str] | None) -> None:
    if result is None:
        return
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
        print(result.stderr, end="" if result.stderr.endswith("\n") else "\n", file=sys.stderr)


def _parse_uploaded_hash(image_list_output: str) -> str | None:
    """Best-effort parser for common `mcumgr image list` output."""
    blocks = re.split(r"(?=image=|\n\s*image:)", image_list_output)
    fallback: str | None = None

    for block in blocks:
        match = HASH_RE.search(block)
        if match is None:
            continue
        digest = match.group(0).lower()
        if fallback is None:
            fallback = digest
        lowered = block.lower()
        if "slot=1" in lowered or "slot: 1" in lowered:
            return digest

    return fallback


def _wait_for_fw(
    fib: HispecFibPcb,
    expected_fw: str,
    timeout_s: float,
    poll_s: float,
) -> None:
    deadline = time.monotonic() + timeout_s
    last_error: Exception | None = None

    while time.monotonic() < deadline:
        try:
            status = fib.status(ip=True)
            print(f"board fw={status.fw} ip={status.ip.active.ip if status.ip else 'unknown'}")
            if status.fw == expected_fw:
                return
        except Exception as exc:
            last_error = exc
        time.sleep(poll_s)

    detail = f": {last_error}" if last_error is not None else ""
    raise TimeoutError(f"timed out waiting for fw={expected_fw}{detail}")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="HISPEC FIB PCB OTA update helper")
    parser.add_argument("--broker", required=True, help="MQTT broker hostname or IPv4 address")
    parser.add_argument("--broker-port", type=int, default=1883)
    parser.add_argument("--device", default="hsfib-tib", choices=DEVICE_NAMES)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--board-host", help="Board IPv4 address; defaults to status ip.active.ip")
    parser.add_argument("--smp-port", type=int, default=1337)
    parser.add_argument("--duration-s", type=int, default=600)
    parser.add_argument("--image", required=True, type=Path,
                        help="MCUboot update image binary; unsigned by observatory policy")
    parser.add_argument("--image-hash", help="Image hash to pass to `mcumgr image test`")
    parser.add_argument("--expected-fw", help="Firmware fw string required before confirmation")
    parser.add_argument("--mcumgr", default="mcumgr", help="MCUmgr-compatible CLI executable")
    parser.add_argument("--wait-s", type=float, default=120.0)
    parser.add_argument("--poll-s", type=float, default=3.0)
    parser.add_argument("--skip-upload", action="store_true")
    parser.add_argument("--skip-reboot", action="store_true")
    parser.add_argument("--skip-confirm", action="store_true")
    parser.add_argument("--confirm-anyway", action="store_true",
                        help="Confirm after reboot without checking --expected-fw")
    parser.add_argument("--dry-run", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)

    if args.duration_s <= 0:
        raise SystemExit("--duration-s must be positive")
    if args.smp_port <= 0 or args.smp_port > 65535:
        raise SystemExit("--smp-port must be in 1..65535")
    if not args.image.exists() and not args.dry_run:
        raise SystemExit(f"image not found: {args.image}")
    if shutil.which(args.mcumgr) is None and not args.dry_run:
        raise SystemExit(f"MCUmgr executable not found: {args.mcumgr}")
    if not args.skip_confirm and not args.expected_fw and not args.confirm_anyway:
        raise SystemExit("--expected-fw is required before confirmation; use --skip-confirm or --confirm-anyway")

    fib = HispecFibPcb(
        args.broker,
        port=args.broker_port,
        device=args.device,
        timeout_s=args.timeout,
        connect=False,
    )
    ota_window_open = False
    try:
        if not args.dry_run:
            fib.connect()
        status = fib.status(ip=True) if not args.dry_run else None
        board_host = args.board_host
        if board_host is None and status is not None and status.ip is not None:
            board_host = status.ip.active.ip
        if not board_host:
            raise SystemExit("board host unavailable; pass --board-host")

        if status is not None:
            print(f"target device={args.device} fw={status.fw} board={status.board} ip={board_host}")
        else:
            print(f"target device={args.device} ip={board_host}")

        if args.dry_run:
            print(f"+ mqtt ota enable=true duration_s={args.duration_s}")
        else:
            fib.set_ota_window(True, duration_s=args.duration_s)
            ota_window_open = True

        image_hash = args.image_hash
        if not args.skip_upload:
            base = _mcumgr_base(args.mcumgr, board_host, args.smp_port)
            _print_completed(_run([*base, "image", "upload", str(args.image)], dry_run=args.dry_run))
            list_result = _run([*base, "image", "list"], dry_run=args.dry_run)
            _print_completed(list_result)
            if image_hash is None and list_result is not None:
                image_hash = _parse_uploaded_hash(list_result.stdout)
            if image_hash is None and args.dry_run:
                image_hash = "DRYRUN_IMAGE_HASH"

        if image_hash is None and not args.skip_upload:
            raise SystemExit("could not determine uploaded image hash; pass --image-hash")
        if image_hash is not None:
            _print_completed(_run([*_mcumgr_base(args.mcumgr, board_host, args.smp_port),
                                   "image", "test", image_hash],
                                  dry_run=args.dry_run))

        if not args.skip_reboot:
            if args.dry_run:
                print("+ mqtt reboot")
            else:
                fib.reboot()
                ota_window_open = False

        if args.expected_fw and not args.skip_reboot and not args.dry_run:
            _wait_for_fw(fib, args.expected_fw, args.wait_s, args.poll_s)

        if not args.skip_confirm:
            if args.dry_run:
                print("+ mqtt ota confirm=true")
            else:
                fib.ota_confirm()

        if args.dry_run:
            print("+ mqtt ota enable=false")
        else:
            fib.set_ota_window(False)
            ota_window_open = False
    finally:
        if ota_window_open:
            try:
                fib.set_ota_window(False)
            except Exception as exc:
                print(f"warning: failed to close OTA window: {exc}", file=sys.stderr)
        fib.close()

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (HispecFibError, subprocess.CalledProcessError, TimeoutError) as exc:
        raise SystemExit(str(exc)) from exc
