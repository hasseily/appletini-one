#!/usr/bin/env python3
"""Deliver a checked image through the board's existing FTP/SD updater.

Use only when JTAG is absent and the Golden UART prompt cannot be caught.
The Golden image is never written. Both upload and readback hashes must match
before the board is restarted into its normal SD firmware updater.
"""
import argparse
import ftplib
import hashlib
import io
import json
from pathlib import Path
import time

import serial

from https_probe_host import OUT, serial_command
from image_manifest import verify_image, ROLE_FIRMWARE


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", type=Path)
    ap.add_argument("--board", required=True)
    ap.add_argument("--port", default="COM13")
    ap.add_argument("--output-dir", type=Path, default=OUT)
    args = ap.parse_args()
    out = args.output_dir
    verify_image(args.image, expected_role=ROLE_FIRMWARE, expected_recovery=True,
                 expected_product="appletini")
    payload = args.image.read_bytes()
    expected = hashlib.sha256(payload).hexdigest()
    out.mkdir(parents=True, exist_ok=True)
    log = []
    with serial.Serial(args.port, 921600, timeout=.05) as uart:
        log.append(serial_command(uart, "ftp on", timeout=1))
        try:
            with ftplib.FTP() as ftp:
                ftp.connect(args.board, timeout=60)
                ftp.login("anonymous", "appletini-checkpoint")
                existing = io.BytesIO()
                try:
                    ftp.retrbinary("RETR /FIRMWARE.BIN", existing.write)
                except ftplib.error_perm as error:
                    if not str(error).startswith("550"):
                        raise
                if existing.tell():
                    backup = out / ("sd-FIRMWARE-before-" + str(int(time.time())) + ".bin")
                    backup.write_bytes(existing.getvalue())
                    print("Preserved previous SD FIRMWARE.BIN:", backup, flush=True)
                progress = 0
                last = 0

                def sent(data):
                    nonlocal progress, last
                    progress += len(data)
                    if progress - last >= 512 * 1024:
                        print(f"Uploaded {progress}/{len(payload)} bytes", flush=True)
                        last = progress

                ftp.storbinary("STOR /FIRMWARE.BIN", io.BytesIO(payload), blocksize=4096, callback=sent)
                received = hashlib.sha256()
                ftp.retrbinary("RETR /FIRMWARE.BIN", received.update, blocksize=4096)
                if received.hexdigest() != expected:
                    raise RuntimeError("Uploaded firmware readback SHA256 mismatch; no reboot")
                print("Verified SD firmware SHA256:", expected, flush=True)
        finally:
            log.append(serial_command(uart, "ftp off", timeout=1))
        log.append(serial_command(uart, "reboot", timeout=15))
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    (out / f"deploy-{stamp}.txt").write_text("".join(log), encoding="utf-8")
    (out / f"deploy-{stamp}.json").write_text(json.dumps({
        "image": str(args.image.resolve()), "sha256": expected,
        "board": args.board, "sd_readback_verified": True,
        "golden_written": False, "method": "existing FTP then SD firmware updater",
    }, indent=2) + "\n", encoding="utf-8")
    print("SD delivery complete; check the running firmware separately.", flush=True)


if __name__ == "__main__":
    main()
