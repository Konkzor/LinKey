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

    print(f"MQTT QEMU assertions OK for tariff_option={args.tariff_option}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
