#!/usr/bin/env python3
"""Validate the QEMU manual WiFi provisioning trigger smoke test."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


PANIC_RE = re.compile(r"Guru Meditation Error|abort\(\) was called|assert failed:|CORRUPT HEAP")


def fail(message: str) -> None:
    print(f"::error::{message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu-log", type=Path, required=True)
    args = parser.parse_args()

    log = args.qemu_log.read_text(errors="replace")

    if "main_task: Calling app_main()" not in log:
        fail("QEMU log does not show app_main() entry")
    if PANIC_RE.search(log):
        fail("QEMU log contains a panic/assert/heap corruption marker")
    if "QEMU manual BOOT/GPIO0 press active" not in log:
        fail("QEMU did not simulate the BOOT/GPIO0 long press")
    if "BLE provisioning is not supported in QEMU emulation" not in log:
        fail("QEMU provisioning stub was not called after the manual request")

    print("QEMU manual provisioning assertions OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
