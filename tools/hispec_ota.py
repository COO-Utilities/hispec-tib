#!/usr/bin/env python3
"""Update one HISPEC controller, or generate lab images from its sysbuild.

MQTT opens the maintenance window and confirms the exact running image. SMP
uploads to the secondary slot and requests a test boot. This tool never changes
bank power mode. Hash-only images require the trusted observatory network.
"""

from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass
import ipaddress
import json
from pathlib import Path
import struct
import sys
import tempfile
import time

from hispec_fibpcb import HispecFibError, HispecFibPCBError, HispecFibPcb
from smpclient import SMPClient
from smpclient.exceptions import SMPBadSequence, SMPClientException
from smpclient.generics import error
from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite
from smpclient.transport.udp import SMPUDPTransport

WORKSPACE = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = WORKSPACE / "hispec-tib/app/build-ota"
# Use the same pinned imgtool and devicetree parser as west, not a PyPI imgtool.
sys.path.insert(0, str(WORKSPACE / "bootloader/mcuboot/scripts"))
sys.path.insert(0, str(WORKSPACE / "zephyr/scripts/dts/python-devicetree/src"))
from devicetree.dtlib import DT  # noqa: E402
from imgtool.image import Image, VerifyResult  # noqa: E402
from imgtool.version import decode_version  # noqa: E402


@dataclass(frozen=True)
class BuildLayout:
    """Image bounds from generated configuration, including offset/trailer space."""

    header_size: int
    slot_size: int
    max_image_size: int
    align: int
    max_align: int
    version: str

    @classmethod
    def read(cls, build: Path) -> BuildLayout:
        config = dict(line.split("=", 1) for line in
                      (build / "app/zephyr/.config").read_text().splitlines()
                      if line.startswith("CONFIG_"))
        boot_config = (build / "mcuboot/zephyr/.config").read_text().splitlines()
        if ("CONFIG_BOOT_SIGNATURE_TYPE_NONE=y" not in boot_config or
                config.get("CONFIG_MCUBOOT_BOOTLOADER_MODE_SWAP_USING_OFFSET") != "y"):
            raise ValueError("requires the hash-only, swap-using-offset HISPEC sysbuild")
        dt = DT(str(build / "app/zephyr/zephyr.dts"))
        slot_size = dt.label2node["slot1_partition"].props["reg"].to_nums()[1]
        return cls(
            header_size=int(config["CONFIG_ROM_START_OFFSET"], 0),
            slot_size=slot_size,
            max_image_size=slot_size - int(config["CONFIG_MCUBOOT_UPDATE_FOOTER_SIZE"], 0),
            align=dt.label2node["flash0"].props["write-block-size"].to_num(),
            max_align=int(config["CONFIG_MCUBOOT_BOOT_MAX_ALIGN"], 0),
            version=config["CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION"].strip('"'),
        )


def read_image(path: Path, layout: BuildLayout) -> tuple[bytes, bytes]:
    """Validate an unpadded hash-only binary and return its MCUboot identity.

    The whole-file upload SHA is a different hash. Restrict the container to
    this build's header + body + SHA256 TLV; a preconfirmed provisioning image
    with a trailer must never become an OTA candidate.
    """
    data = path.read_bytes()
    if path.suffix.lower() != ".bin" or not 32 <= len(data) <= layout.max_image_size:
        raise ValueError(f"OTA needs a .bin image of 32..{layout.max_image_size} bytes")
    magic, load, header, protected, body, flags = struct.unpack_from("<IIHHII", data)
    body_end = header + body
    if (magic != 0x96F3B83D or load != 0 or header != layout.header_size or
            protected != 0 or flags != 0 or body == 0 or len(data) != body_end + 40 or
            data[body_end:body_end + 8] != struct.pack("<HHBBH", 0x6907, 40, 0x10, 0, 32)):
        raise ValueError("expected an unpadded, hash-only zephyr.signed.bin container")
    result, _version, digest, _signature = Image.verify(str(path), None)
    if result != VerifyResult.OK or digest is None:
        raise ValueError(f"MCUboot image verification failed: {result.name}")
    return data, digest


def make_fixtures(build: Path, output: Path, layout: BuildLayout) -> None:
    """Append deterministic non-erased bytes before hashing; never alter code/RAM.

    The maximum image consumes the full generated upload limit. The oversized
    image is otherwise valid. The corrupt image changes the last body byte,
    demonstrating that the extension is covered by the image hash.
    """
    raw = (build / "app/zephyr/zephyr.bin").read_bytes()
    target_raw_size = layout.max_image_size - 40  # SHA256 TLV including its headers.
    if len(raw) > target_raw_size:
        raise ValueError("application already exceeds the generated image limit")
    output.mkdir(parents=True, exist_ok=True)
    pattern = bytes(range(251))  # No 0xff erased-flash bytes.
    manifest = {"max_image_size": layout.max_image_size, "images": {}}
    with tempfile.TemporaryDirectory(dir=output) as temporary:
        raw_path = Path(temporary) / "fixture.bin"
        for name, extra in (("maximum", 0), ("oversized", layout.align)):
            fill_size = target_raw_size + extra - len(raw)
            raw_path.write_bytes(raw + (pattern * ((fill_size + 250) // 251))[:fill_size])
            fixture = Image(version=decode_version(layout.version),
                            header_size=layout.header_size, align=layout.align,
                            max_align=layout.max_align, slot_size=layout.slot_size)
            fixture.load(str(raw_path))
            fixture.create(key=None, public_key_format="hash", enckey=None)
            path = output / f"{name}.bin"
            fixture.save(str(path))
            result, _version, digest, _signature = Image.verify(str(path), None)
            if result != VerifyResult.OK or path.stat().st_size != layout.max_image_size + extra:
                raise ValueError(f"unexpected fixture size/hash: {path}")
            manifest["images"][name] = {"bytes": path.stat().st_size,
                                         "image_hash": digest.hex()}
    maximum, digest = read_image(output / "maximum.bin", layout)
    damaged = bytearray(maximum)
    damaged[-41] ^= 1  # Last hashed body byte, just before the SHA256 TLV.
    corrupt_path = output / "corrupt-tail.bin"
    corrupt_path.write_bytes(damaged)
    if Image.verify(str(corrupt_path), None)[0] != VerifyResult.INVALID_HASH:
        raise ValueError("corrupt fixture unexpectedly passed verification")
    manifest["images"]["corrupt-tail"] = {"bytes": len(damaged), "valid": False}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Created fixtures in {output}; maximum={len(maximum)} bytes, hash={digest.hex()}")


async def upload_test(ip: str, data: bytes, digest: bytes, running_hash: str,
                      duration_s: int) -> None:
    """Upload with three bounded transport attempts, then mark only a test boot.

    Restarting the same upload SHA lets Zephyr return its saved offset. A lost
    test-request reply is recovered by reading the pending flag before retrying.
    MCUboot validates the full body before executing the candidate at reboot.
    """
    async with asyncio.timeout(duration_s - 5):
        for attempt in range(3):
            try:
                async with SMPClient(SMPUDPTransport(mtu=1024), ip, timeout_s=5) as smp:
                    states = await smp.request(ImageStatesRead())
                    if error(states):
                        raise RuntimeError(f"image-state read rejected: {states}")
                    primary = next((s for s in states.images if s.slot == 0), None)
                    if (primary is None or primary.hash != bytes.fromhex(running_hash) or
                            not primary.active or not primary.confirmed):
                        raise RuntimeError("SMP target differs from the confirmed MQTT target")
                    secondary = next((s for s in states.images if s.slot == 1), None)
                    if secondary is not None and secondary.pending:
                        if secondary.hash == digest and not secondary.permanent:
                            return
                        raise RuntimeError("secondary slot already has another/permanent pending image")
                    next_progress = 0
                    async for offset in smp.upload(data, slot=0, upgrade=False,
                                                   first_timeout_s=30, use_sha=True):
                        if not 0 <= offset <= len(data):
                            raise RuntimeError("device returned an invalid upload offset")
                        percent = offset * 100 // len(data)
                        if percent >= next_progress:
                            print(f"Upload {percent}% ({offset}/{len(data)} bytes)", flush=True)
                            next_progress = (percent // 10 + 1) * 10
                    states = await smp.request(ImageStatesRead())
                    if error(states):
                        raise RuntimeError(f"uploaded image-state read rejected: {states}")
                    secondary = next((s for s in states.images if s.slot == 1), None)
                    if secondary is None or secondary.hash != digest or not secondary.bootable:
                        raise RuntimeError("secondary slot does not report the uploaded image hash")
                    states = await smp.request(ImageStatesWrite(hash=digest, confirm=False))
                    if error(states):
                        raise RuntimeError(f"test boot rejected: {states}")
                    secondary = next((s for s in states.images if s.slot == 1), None)
                    if (secondary is None or secondary.hash != digest or
                            not secondary.pending or secondary.permanent):
                        raise RuntimeError("device did not mark the candidate for a test boot")
                    return
            except (TimeoutError, OSError, SMPBadSequence) as exc:
                if attempt == 2:
                    raise
                print(f"SMP transport retry {attempt + 1}/2: {exc}", file=sys.stderr)


def wait_for_trial(pcb: HispecFibPcb, digest: bytes, old_boots: int,
                   timeout_s: int, skip_confirm: bool) -> None:
    """Use fresh correlated MQTT replies after reboot; never confirm another hash."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            board = pcb.status()
            ota = pcb.ota()
        except HispecFibError:
            # The old image rejects commands during the delayed reboot. A new
            # image can also be temporarily unreachable while MQTT reconnects.
            time.sleep(1)
            continue
        if board.boots == old_boots:
            time.sleep(1)
            continue
        try:
            if ota.image_hash != digest.hex():
                raise RuntimeError("reboot returned another image (rollback or rejected candidate)")
            if not board.board_ok:
                raise RuntimeError("candidate reports board setup failure; leaving it unconfirmed")
            if board.board == "tib":
                bank = pcb.laser_bankpower()
                if bank.mode != "override_off" or bank.powered:
                    raise RuntimeError("candidate did not keep the laser bank off; refusing confirmation")
            if skip_confirm:
                if ota.confirmed:
                    raise RuntimeError("rollback test requires an unconfirmed candidate")
                print(f"Trial running; confirmation skipped. Rollback in {ota.trial_remaining_s}s.")
                return
            if not ota.confirmed:
                # If the ACK is lost, the next loop reads confirmed state before
                # deciding whether an identical confirmation must be retried.
                pcb.confirm_image(digest.hex())
                ota = pcb.ota()
            if ota.image_hash != digest.hex() or not ota.confirmed:
                raise RuntimeError("exact running-image confirmation was not verified")
            print(f"Confirmed {digest.hex()}. Bank mode remains override_off on TIB; operator restores auto.")
            return
        except HispecFibPCBError:
            raise  # Explicit command rejection is not a transport retry.
        except HispecFibError:
            time.sleep(1)
    raise TimeoutError("no verified confirmation before host timeout; an unconfirmed trial will roll back")


def update(args: argparse.Namespace, layout: BuildLayout) -> None:
    """Validate locally first, then perform the bounded MQTT/SMP update sequence."""
    data, digest = read_image(args.image, layout)
    print(f"Image {len(data)}/{layout.max_image_size} bytes; MCUboot hash {digest.hex()}")
    if args.dry_run:
        print("Dry run: local image verified; no network connections or device changes.")
        return
    if args.broker is None:
        raise ValueError("--broker is required for an update")
    with HispecFibPcb(args.broker, port=args.broker_port, device=args.device) as pcb:
        if pcb.help().device != args.device:
            raise RuntimeError("MQTT device identity mismatch")
        board = pcb.status(ip=True)
        ota = pcb.ota()
        if not board.board_ok or board.ip is None or not board.ip.active.ready:
            raise RuntimeError("target board or network is not ready")
        ip = str(ipaddress.IPv4Address(board.ip.active.ip))
        if args.ip is not None and ip != args.ip:
            raise RuntimeError(f"MQTT target reports {ip}, not --ip {args.ip}")
        if layout.max_image_size != ota.max_image_size or len(data) > ota.max_image_size:
            raise RuntimeError("target upload limit differs from this sysbuild")
        if not ota.confirmed or ota.active:
            raise RuntimeError("target must be confirmed with no active window or pending image")
        if ota.image_hash == digest.hex():
            print("This exact image is already running and confirmed.")
            return
        if board.board == "tib":
            bank = pcb.laser_bankpower()
            if bank.mode != "override_off" or bank.powered:
                raise RuntimeError("operator must first stop experiments and set laser/bankpower override_off")
        try:
            pcb.set_ota_window(True, duration_s=args.duration_s)
            asyncio.run(upload_test(ip, data, digest, ota.image_hash, args.duration_s))
        finally:
            # Closing also covers an open ACK lost in transit. Failure cannot
            # reopen access; firmware expires the window independently.
            try:
                pcb.set_ota_window(False)
            except HispecFibError as exc:
                print(f"Could not acknowledge window close; it will expire: {exc}", file=sys.stderr)
        try:
            pcb.reboot()
        except HispecFibPCBError:
            raise
        except HispecFibError:
            print("Reboot ACK lost; checking fresh boot state.", file=sys.stderr)
        wait_for_trial(pcb, digest, board.boots, args.boot_timeout_s, args.skip_confirm)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    upload = sub.add_parser("update", help="verify, upload, test boot and confirm one image")
    upload.add_argument("image", type=Path)
    upload.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    upload.add_argument("--broker")
    upload.add_argument("--broker-port", type=int, default=1883)
    upload.add_argument("--device", default="hsfib-tib")
    upload.add_argument("--ip", help="optional IPv4 cross-check against fresh MQTT status")
    upload.add_argument("--duration-s", type=int, default=600)
    upload.add_argument("--boot-timeout-s", type=int, default=180)
    upload.add_argument("--dry-run", action="store_true")
    upload.add_argument("--skip-confirm", action="store_true", help="lab rollback trial")
    fixtures = sub.add_parser("fixtures", help="generate maximum, oversized and corrupt lab images")
    fixtures.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    fixtures.add_argument("--output", type=Path, help="default: BUILD/test-images")
    args = parser.parse_args()
    try:
        layout = BuildLayout.read(args.build_dir)
        if args.action == "fixtures":
            make_fixtures(args.build_dir, args.output or args.build_dir / "test-images", layout)
        else:
            if not 30 <= args.duration_s <= 1800 or not 1 <= args.boot_timeout_s <= 300:
                raise ValueError("duration must be 30..1800s and boot timeout 1..300s")
            update(args, layout)
    except (OSError, ValueError, RuntimeError, SMPClientException) as exc:
        print(f"OTA failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
