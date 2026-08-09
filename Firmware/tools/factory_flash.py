#!/usr/bin/env python3
"""Generate and flash per-device Linkey factory configuration."""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import secrets
import shutil
import string
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import quote


REPO_FIRMWARE_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = REPO_FIRMWARE_DIR.parent
PARTITION_CSV = REPO_FIRMWARE_DIR / "partitions.csv"
SCHEMA_HEADER = REPO_FIRMWARE_DIR / "main" / "factory_config_schema.h"
QUICK_START_TEMPLATE = REPO_ROOT / "Templates" / "Guide démarrage rapide" / "linkey_quick_start_guide_template.html"
FACTORY_ROOT = REPO_FIRMWARE_DIR / "factory" / "devices"
DEFAULT_BUILD_DIR = REPO_FIRMWARE_DIR / "build-factory"
DEFAULT_SDKCONFIG = REPO_FIRMWARE_DIR / "sdkconfig.factory"
PROV_QR_VERSION = "v1"
PROV_TRANSPORT = "ble"
QRCODE_BASE_URL = "https://espressif.github.io/esp-jumpstart/qrcode.html"
SECRET_ALPHABET = string.ascii_letters + string.digits


def read_schema() -> dict[str, str]:
    values: dict[str, str] = {}
    aliases: dict[str, str] = {}
    quoted_pattern = re.compile(r'#define\s+(LINKEY_FACTORY_[A-Z0-9_]+)\s+"([^"]+)"')
    alias_pattern = re.compile(r"#define\s+(LINKEY_FACTORY_[A-Z0-9_]+)\s+(LINKEY_FACTORY_[A-Z0-9_]+)")
    with SCHEMA_HEADER.open("r", encoding="utf-8") as f:
        for line in f:
            stripped = line.strip()
            match = quoted_pattern.match(stripped)
            if match:
                values[match.group(1)] = match.group(2)
                continue

            match = alias_pattern.match(stripped)
            if match:
                aliases[match.group(1)] = match.group(2)

    while aliases:
        resolved = False
        for name, target in list(aliases.items()):
            if target in values:
                values[name] = values[target]
                del aliases[name]
                resolved = True
        if not resolved:
            break

    required = {
        "LINKEY_FACTORY_PARTITION_LABEL",
        "LINKEY_FACTORY_NAMESPACE",
        "LINKEY_FACTORY_KEY_MQTT_PASSWORD",
        "LINKEY_FACTORY_KEY_BLE_POP",
        "LINKEY_FACTORY_FALLBACK_MQTT_PASSWORD",
        "LINKEY_FACTORY_FALLBACK_BLE_POP",
    }
    missing = sorted(required - values.keys())
    if missing:
        raise SystemExit(f"Factory schema is missing: {', '.join(missing)}")
    return values


def run(cmd: list[str], cwd: Path | None = None, capture: bool = False) -> str:
    print("+ " + " ".join(cmd))
    result = subprocess.run(
        cmd,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )
    return result.stdout if capture and result.stdout else ""


def find_idf_path() -> Path:
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise SystemExit("IDF_PATH is not set. Source ESP-IDF export.sh before running this tool.")
    return Path(idf_path)


def read_mac(port: str, esptool: str) -> str:
    output = run([
        esptool,
        "--chip",
        "esp32",
        "-p",
        port,
        "--before",
        "default_reset",
        "--after",
        "no_reset",
        "read_mac",
    ], capture=True)
    match = re.search(r"MAC:\s*([0-9a-fA-F:]{12,17})", output)
    if not match:
        raise SystemExit("Could not parse ESP32 MAC address from esptool output.")
    return match.group(1).replace(":", "").lower()


def random_secret(length: int) -> str:
    return "".join(secrets.choice(SECRET_ALPHABET) for _ in range(length))


def device_paths(mac: str) -> dict[str, Path]:
    root = FACTORY_ROOT / mac
    return {
        "root": root,
        "json": root / "factory_config.json",
        "csv": root / "factory_config.csv",
        "bin": root / "factory_config.bin",
        "quick_start": root / "quick_start.txt",
        "quick_start_html": root / "quick_start.html",
        "quick_start_pdf": root / "quick_start.pdf",
    }


def load_or_create_config(mac: str, schema: dict[str, str]) -> dict:
    paths = device_paths(mac)
    suffix = mac[-6:]

    if paths["json"].exists():
        with paths["json"].open("r", encoding="utf-8") as f:
            return json.load(f)

    ble_name = f"Linkey_{suffix.upper()}"
    mqtt_username = f"linkey_{suffix}"
    ble_pop = random_secret(12)
    mqtt_password = random_secret(12)
    qr_payload = json.dumps(
        {
            "ver": PROV_QR_VERSION,
            "name": ble_name,
            "pop": ble_pop,
            "transport": PROV_TRANSPORT,
        },
        separators=(",", ":"),
    )

    return {
        "mac": mac,
        "device_id": f"linkey_{suffix}",
        "ble_name": ble_name,
        schema["LINKEY_FACTORY_KEY_BLE_POP"]: ble_pop,
        "mqtt_username": mqtt_username,
        schema["LINKEY_FACTORY_KEY_MQTT_PASSWORD"]: mqtt_password,
        "mqtt_topic_prefix": f"linkey/{suffix}",
        "mqtt_topics": {
            "state": f"linkey/{suffix}/state",
            "status": f"linkey/{suffix}/status",
            "debug_request": f"linkey/{suffix}/debug/request",
            "debug_tic_frame": f"linkey/{suffix}/debug/tic_frame",
        },
        "provisioning": {
            "qr_payload": qr_payload,
            "url": f"{QRCODE_BASE_URL}?data={quote(qr_payload, safe='')}",
        },
    }


def write_config_files(config: dict, schema: dict[str, str], chrome: str | None) -> None:
    paths = device_paths(config["mac"])
    paths["root"].mkdir(parents=True, exist_ok=True)

    with paths["json"].open("w", encoding="utf-8") as f:
        json.dump(config, f, indent=2)
        f.write("\n")

    with paths["csv"].open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["key", "type", "encoding", "value"])
        writer.writerow([schema["LINKEY_FACTORY_NAMESPACE"], "namespace", "", ""])
        writer.writerow([
            schema["LINKEY_FACTORY_KEY_MQTT_PASSWORD"],
            "data",
            "string",
            config[schema["LINKEY_FACTORY_KEY_MQTT_PASSWORD"]],
        ])
        writer.writerow([
            schema["LINKEY_FACTORY_KEY_BLE_POP"],
            "data",
            "string",
            config[schema["LINKEY_FACTORY_KEY_BLE_POP"]],
        ])

    paths["quick_start"].write_text(quick_start_text(config, schema), encoding="utf-8")
    write_quick_start_guide(config, schema, paths, chrome)


def write_quick_start_guide(config: dict, schema: dict[str, str],
                            paths: dict[str, Path], chrome: str | None) -> None:
    if not QUICK_START_TEMPLATE.exists():
        print(f"Warning: quick start template not found: {QUICK_START_TEMPLATE}")
        return

    template = QUICK_START_TEMPLATE.read_text(encoding="utf-8")
    provisioning_qr_url = (
        "https://api.qrserver.com/v1/create-qr-code/"
        f"?size=512x512&margin=12&data={quote(config['provisioning']['url'], safe='')}"
    )
    logo_path = (REPO_ROOT / "Doc" / "images" / "logo_linkey.png").resolve().as_uri()
    template_image_dir = QUICK_START_TEMPLATE.parent / "images"
    google_play_badge = (template_image_dir / "google-play-badge.png").resolve().as_uri()
    app_store_badge = (template_image_dir / "app-store-badge.svg").resolve().as_uri()

    html = template
    html = html.replace("../../Doc/images/logo_linkey.png", logo_path)
    html = html.replace("images/google-play-badge.png", google_play_badge)
    html = html.replace("images/app-store-badge.svg", app_store_badge)
    html = html.replace("{{mqtt_username}}", config["mqtt_username"])
    html = html.replace("{{mqtt_user}}", config["mqtt_username"])
    html = html.replace(
        "{{mqtt_password}}",
        config[schema["LINKEY_FACTORY_KEY_MQTT_PASSWORD"]],
    )
    html = html.replace(
        "{{mqtt_pass}}",
        config[schema["LINKEY_FACTORY_KEY_MQTT_PASSWORD"]],
    )
    html = html.replace("Linkey-&lt;MAC&gt;", config["ble_name"])
    html = html.replace("Linkey_&lt;MAC&gt;", config["ble_name"])
    html = html.replace("Linkey_MAC", config["ble_name"])
    html = html.replace(
        '<div class="provision-qr">QR CODE<br>PROVISIONING</div>',
        (
            '<div class="provision-qr">'
            f'<img src="{provisioning_qr_url}" alt="QR code de provisioning Linkey">'
            '</div>'
        ),
    )

    paths["quick_start_html"].write_text(html, encoding="utf-8")
    if chrome:
        try:
            generate_quick_start_pdf(paths["quick_start_html"], paths["quick_start_pdf"], chrome)
        except subprocess.CalledProcessError as exc:
            print(f"Warning: quick start PDF generation failed ({exc}); HTML guide was generated.")
    else:
        print("Warning: Chrome/Chromium not found; quick start PDF was not generated.")


def find_chrome(explicit_path: str | None) -> str | None:
    if explicit_path:
        return explicit_path

    for name in ("google-chrome", "chromium", "chromium-browser"):
        path = shutil.which(name)
        if path:
            return path
    return None


def generate_quick_start_pdf(html_path: Path, pdf_path: Path, chrome: str) -> None:
    with tempfile.TemporaryDirectory(prefix="linkey-chrome-") as user_data_dir:
        run([
            chrome,
            "--headless",
            "--disable-gpu",
            "--no-sandbox",
            "--disable-dev-shm-usage",
            "--disable-crash-reporter",
            "--disable-crashpad",
            "--allow-file-access-from-files",
            f"--user-data-dir={user_data_dir}",
            f"--print-to-pdf={pdf_path}",
            "--print-to-pdf-no-header",
            str(html_path.resolve().as_uri()),
        ])


def quick_start_text(config: dict, schema: dict[str, str]) -> str:
    topics = config["mqtt_topics"]
    return "\n".join(
        [
            "Linkey setup information",
            "",
            f"MAC: {config['mac']}",
            f"BLE service name: {config['ble_name']}",
            f"BLE Proof of Possession: {config[schema['LINKEY_FACTORY_KEY_BLE_POP']]}",
            f"Provisioning QR content: {config['provisioning']['qr_payload']}",
            f"Provisioning URL: {config['provisioning']['url']}",
            "",
            "Home Assistant / MQTT",
            f"MQTT username: {config['mqtt_username']}",
            f"MQTT password: {config[schema['LINKEY_FACTORY_KEY_MQTT_PASSWORD']]}",
            f"MQTT topic prefix: {config['mqtt_topic_prefix']}",
            f"State topic: {topics['state']}",
            f"Availability topic: {topics['status']}",
            "",
        ]
    )


def partition_info(schema: dict[str, str]) -> tuple[int, int]:
    with PARTITION_CSV.open("r", encoding="utf-8", newline="") as f:
        for row in csv.reader(f):
            if not row or row[0].strip().startswith("#"):
                continue
            name = row[0].strip()
            if name == schema["LINKEY_FACTORY_PARTITION_LABEL"]:
                offset = int(row[3].strip(), 0)
                size = parse_size(row[4].strip())
                return offset, size
    raise SystemExit(
        f"Partition {schema['LINKEY_FACTORY_PARTITION_LABEL']} not found in {PARTITION_CSV}."
    )


def parse_size(value: str) -> int:
    value = value.strip()
    lower = value.lower()
    if lower.endswith("k"):
        return int(lower[:-1], 0) * 1024
    if lower.endswith("m"):
        return int(lower[:-1], 0) * 1024 * 1024
    return int(value, 0)


def generate_nvs_binary(config: dict, schema: dict[str, str], idf_path: Path) -> Path:
    paths = device_paths(config["mac"])
    nvs_gen = idf_path / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
    if not nvs_gen.exists():
        raise SystemExit(f"nvs_partition_gen.py not found at {nvs_gen}")

    _offset, size = partition_info(schema)
    run([sys.executable, str(nvs_gen), "generate", str(paths["csv"]), str(paths["bin"]), hex(size)])
    return paths["bin"]


def idf_project_args(args: argparse.Namespace) -> list[str]:
    return [
        args.idf,
        "-B",
        str(args.build_dir),
        "-D",
        f"SDKCONFIG={args.sdkconfig}",
    ]


def print_setup_info(config: dict, schema: dict[str, str], quick_start_path: Path) -> None:
    print("")
    print(quick_start_text(config, schema), end="")
    print(f"Saved setup sheet: {quick_start_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", required=True, help="Serial port of the ESP32.")
    parser.add_argument("--idf", default="idf.py", help="idf.py command to use.")
    parser.add_argument("--esptool", default="esptool.py", help="esptool.py command to use.")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR,
                        help="Factory build directory to use.")
    parser.add_argument("--sdkconfig", type=Path, default=DEFAULT_SDKCONFIG,
                        help="Factory sdkconfig file to use.")
    parser.add_argument("--no-build", action="store_true", help="Skip idf.py build.")
    parser.add_argument("--no-flash", action="store_true", help="Generate files and print setup info only.")
    parser.add_argument("--factory-only", action="store_true", help="Flash only the factory_data partition.")
    parser.add_argument("--chrome", help="Chrome/Chromium executable used to render the quick start PDF.")
    parser.add_argument("--no-guide-pdf", action="store_true", help="Generate quick start HTML but skip PDF rendering.")
    args = parser.parse_args()

    idf_path = find_idf_path()
    schema = read_schema()
    mac = read_mac(args.port, args.esptool)
    config = load_or_create_config(mac, schema)
    chrome = None if args.no_guide_pdf else find_chrome(args.chrome)
    write_config_files(config, schema, chrome)
    factory_bin = generate_nvs_binary(config, schema, idf_path)

    if not args.no_build:
        run(idf_project_args(args) + ["build"], cwd=REPO_FIRMWARE_DIR)

    if not args.no_flash:
        offset, _size = partition_info(schema)
        factory_flash_cmd = [
            args.esptool,
            "--chip",
            "esp32",
            "-p",
            args.port,
            "--before",
            "no_reset",
            "--after",
            "hard_reset" if args.factory_only else "no_reset",
            "write_flash",
            hex(offset),
            str(factory_bin),
        ]
        run(factory_flash_cmd)

        if not args.factory_only:
            run(idf_project_args(args) + ["-p", args.port, "flash"], cwd=REPO_FIRMWARE_DIR)

    print_setup_info(config, schema, device_paths(mac)["quick_start"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
