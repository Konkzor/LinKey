#!/usr/bin/env python3
"""Validate MQTT messages captured during a QEMU emulation smoke test."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


COMMON_DISCOVERY_COMPONENTS = {"iinst", "vcap", "papp", "uptime"}

TARIFF_DISCOVERY_COMPONENTS = {
    "base": {"base"},
    "hphc": {"hchc", "hchp"},
    "ejp": {"ejphn", "ejphpm"},
    "tempo": {
        "bbrhcjb",
        "bbrhpjb",
        "bbrhcjw",
        "bbrhpjw",
        "bbrhcjr",
        "bbrhpjr",
    },
}

COMMON_STATE_FIELDS = {"iinst", "papp", "vcap", "uptime"}
QEMU_WIFI_STA_MAC = "aabbccddeeff"
DEBUG_REQUEST_TOPIC = "linkey/debug/request"
DEBUG_REQUEST_PAYLOAD = "GET_TIC_FRAME"
DEBUG_FRAME_TOPIC = "linkey/debug/tic_frame"
DEBUG_FRAME_TOPIC_BYTES = b"linkey/debug/tic_frame "

EXPECTED_DEBUG_FRAMES = {
    "base": (
        b"\x02"
        b"\nADCO 123456789012 G\r"
        b"\nOPTARIF BASE 0\r"
        b"\nISOUSC 30 9\r"
        b"\nBASE 012345678 /\r"
        b"\nPTEC TH.. $\r"
        b"\nIINST 005 \\\r"
        b"\nADPS 030 ;\r"
        b"\nIMAX 090 H\r"
        b"\nPAPP 00690 0\r"
        b"\nHHPHC A ,\r"
        b"\nMOTDETAT 000000 B\r"
    ),
    "hphc": (
        b"\x02"
        b"\nADCO 123456789012 G\r"
        b"\nOPTARIF HC.. <\r"
        b"\nISOUSC 30 9\r"
        b"\nHCHC 000123456 [\r"
        b"\nHCHP 000023456 '\r"
        b"\nPTEC HP..  \r"
        b"\nIINST 005 \\\r"
        b"\nADPS 030 ;\r"
        b"\nIMAX 090 H\r"
        b"\nPAPP 00690 0\r"
        b"\nHHPHC A ,\r"
        b"\nMOTDETAT 000000 B\r"
    ),
    "ejp": (
        b"\x02"
        b"\nADCO 123456789012 G\r"
        b"\nOPTARIF EJP  T\r"
        b"\nISOUSC 30 9\r"
        b"\nEJPHN 000123456 :\r"
        b"\nEJPHPM 000023456 H\r"
        b"\nPEJP 30 R\r"
        b"\nPTEC HN.. ^\r"
        b"\nIINST 005 \\\r"
        b"\nADPS 030 ;\r"
        b"\nIMAX 090 H\r"
        b"\nPAPP 00690 0\r"
        b"\nHHPHC A ,\r"
        b"\nMOTDETAT 000000 B\r"
    ),
    "tempo": (
        b"\x02"
        b"\nADCO 123456789012 G\r"
        b"\nOPTARIF BBR( S\r"
        b"\nISOUSC 30 9\r"
        b"\nBBRHCJB 000123456 2\r"
        b"\nBBRHPJB 000023456 >\r"
        b"\nBBRHCJW 000023457 G\r"
        b"\nBBRHPJW 000023458 U\r"
        b"\nBBRHCJR 000023459 D\r"
        b"\nBBRHPJR 000023460 I\r"
        b"\nPTEC HPJB P\r"
        b"\nDEMAIN ---- \"\r"
        b"\nIINST 005 \\\r"
        b"\nADPS 030 ;\r"
        b"\nIMAX 090 H\r"
        b"\nPAPP 00690 0\r"
        b"\nHHPHC A ,\r"
        b"\nMOTDETAT 000000 B\r"
    ),
}

DISCOVERY_TOPIC_RE = re.compile(r"^homeassistant/device/linkey_([0-9a-f]{12})/config$")
PANIC_RE = re.compile(r"Guru Meditation Error|abort\(\) was called|assert failed:|CORRUPT HEAP")


def fail(message: str) -> None:
    print(f"::error::{message}", file=sys.stderr)
    raise SystemExit(1)


def load_mqtt_messages(path: Path) -> list[tuple[str, str]]:
    messages: list[tuple[str, str]] = []
    for line in path.read_text(errors="replace").splitlines():
        if not line.strip():
            continue
        try:
            topic, payload = line.split(maxsplit=1)
        except ValueError:
            continue
        messages.append((topic, payload))
    return messages


def parse_json(topic: str, payload: str) -> dict:
    try:
        value = json.loads(payload)
    except json.JSONDecodeError as exc:
        fail(f"MQTT payload on {topic} is not valid JSON: {exc}")
    if not isinstance(value, dict):
        fail(f"MQTT payload on {topic} is JSON but not an object")
    return value


def assert_qemu_log(path: Path) -> None:
    log = path.read_text(errors="replace")
    if "main_task: Calling app_main()" not in log:
        fail("QEMU log does not show app_main() entry")
    if "QEMU Ethernet got IP:" not in log:
        fail("QEMU log does not show QEMU Ethernet IP acquisition")
    if PANIC_RE.search(log):
        fail("QEMU log contains a panic/assert/heap corruption marker")


def assert_status(messages: list[tuple[str, str]]) -> None:
    if ("linkey/status", "online") not in messages:
        fail("Missing exact MQTT status message: linkey/status online")


def assert_discovery(
    messages: list[tuple[str, str]],
    tariff_option: str,
) -> None:
    discovery_messages = [
        (topic, payload, DISCOVERY_TOPIC_RE.match(topic))
        for topic, payload in messages
        if topic.startswith("homeassistant/device/")
    ]
    discovery_messages = [
        (topic, payload, match)
        for topic, payload, match in discovery_messages
        if match is not None
    ]
    if not discovery_messages:
        fail("Missing discovery topic homeassistant/device/linkey_<mac>/config")

    topic, payload, match = discovery_messages[-1]
    assert match is not None
    topic_mac = match.group(1)

    if topic_mac != QEMU_WIFI_STA_MAC:
        fail(f"Discovery topic MAC mismatch: expected {QEMU_WIFI_STA_MAC}, got {topic_mac}")

    config = parse_json(topic, payload)
    dev = config.get("dev")
    if not isinstance(dev, dict):
        fail("Discovery payload missing dev object")

    expected_device_id = f"linkey_{topic_mac}"
    if dev.get("ids") != expected_device_id:
        fail(f"Discovery dev.ids mismatch: expected {expected_device_id!r}")
    if dev.get("sn") != topic_mac:
        fail(f"Discovery dev.sn mismatch: expected {topic_mac!r}")
    if config.get("stat_t") != "linkey/state":
        fail("Discovery stat_t must be exactly linkey/state")
    if config.get("avty_t") != "linkey/status":
        fail("Discovery avty_t must be exactly linkey/status")

    components = config.get("cmps")
    if not isinstance(components, dict):
        fail("Discovery payload missing cmps object")

    expected_components = COMMON_DISCOVERY_COMPONENTS | TARIFF_DISCOVERY_COMPONENTS[tariff_option]
    missing = sorted(expected_components - set(components))
    if missing:
        fail(f"Discovery payload missing components for {tariff_option}: {', '.join(missing)}")

    for component in expected_components:
        uniq_id = components[component].get("uniq_id") if isinstance(components[component], dict) else None
        if not isinstance(uniq_id, str) or not uniq_id.startswith(f"linkey_{topic_mac}_"):
            fail(f"Discovery component {component} has invalid uniq_id: {uniq_id!r}")


def assert_state(messages: list[tuple[str, str]]) -> None:
    state_payloads = [payload for topic, payload in messages if topic == "linkey/state"]
    if not state_payloads:
        fail("Missing exact MQTT state topic: linkey/state")

    state = parse_json("linkey/state", state_payloads[-1])
    missing = sorted(COMMON_STATE_FIELDS - set(state))
    if missing:
        fail(f"State payload missing common fields: {', '.join(missing)}")


def assert_debug_frame(messages: list[tuple[str, str]], mqtt_log: Path, tariff_option: str) -> None:
    if (DEBUG_REQUEST_TOPIC, DEBUG_REQUEST_PAYLOAD) not in messages:
        fail(f"Missing exact debug request echo: {DEBUG_REQUEST_TOPIC} {DEBUG_REQUEST_PAYLOAD}")

    if not any(topic == DEBUG_FRAME_TOPIC for topic, _ in messages):
        fail(f"Missing exact MQTT debug TIC frame topic: {DEBUG_FRAME_TOPIC}")

    log_bytes = mqtt_log.read_bytes()
    frame_start = log_bytes.rfind(DEBUG_FRAME_TOPIC_BYTES)
    if frame_start < 0:
        fail(f"Missing debug TIC frame marker in MQTT log: {DEBUG_FRAME_TOPIC}")

    frame_bytes = log_bytes[frame_start + len(DEBUG_FRAME_TOPIC_BYTES):]
    expected_frame = EXPECTED_DEBUG_FRAMES[tariff_option]
    if not frame_bytes.startswith(expected_frame):
        fail(f"Debug TIC frame payload does not exactly match mocked {tariff_option} frame")

    next_byte_pos = len(expected_frame)
    if len(frame_bytes) > next_byte_pos and frame_bytes[next_byte_pos:next_byte_pos + 1] != b"\n":
        fail("Debug TIC frame payload has unexpected trailing bytes")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tariff-option", choices=sorted(TARIFF_DISCOVERY_COMPONENTS), required=True)
    parser.add_argument("--mqtt-log", type=Path, required=True)
    parser.add_argument("--qemu-log", type=Path, required=True)
    args = parser.parse_args()

    messages = load_mqtt_messages(args.mqtt_log)
    if not messages:
        fail("MQTT log is empty")

    assert_qemu_log(args.qemu_log)
    assert_status(messages)
    assert_discovery(messages, args.tariff_option)
    assert_state(messages)
    assert_debug_frame(messages, args.mqtt_log, args.tariff_option)

    print(f"MQTT QEMU assertions OK for tariff_option={args.tariff_option}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
