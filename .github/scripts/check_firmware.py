#!/usr/bin/env python3
"""
Checks on the built firmware, used by the Tests workflow (build and network-surface jobs).
Adapted from SimplrSSH's check_firmware.py.

  check_firmware.py size <size.json> [--board tab5|waveshare] [--budget BYTES]
      Static internal RAM (DIRAM: .data + .bss + IRAM code placed in DIRAM), from
      `python -m esp_idf_size --format json`, must stay within the board's budget
      (AI_RULES.md, Memory and tasks).

  check_firmware.py warnings <build-log>
      No compiler warnings in main/, except KNOWN_WARNINGS (reported, not failed).

  check_firmware.py secrets <file>...
      No private keys or API keys in the images (nothing secret is ever compiled in).

  check_firmware.py surface <nm-output> <sdkconfig>
      The linked firmware has no listening sockets, servers, OTA of the radio, Bluetooth,
      mDNS or access point, and does have TLS certificate-bundle verification, station mode
      and the WiFi driver kept from storing passwords; the generated sdkconfig keeps the
      attack surface closed (AI_RULES.md, Scope and security).

Each prints GitHub annotations for problems and exits 1 if there are any.
"""

import json
import os
import re
import sys

# Static internal RAM ceiling per board. The ESP32-P4's DIRAM is 563 KB; the radio used
# 133,746 bytes statically on the Tab5 when this was set (2026-10-20: LVGL, esp_hosted,
# WiFi/LWIP, mbedTLS, the MP3 decoder). The headroom absorbs toolchain noise; raise it only
# deliberately, with the reason.
BUDGETS = {"tab5": 140 * 1024, "waveshare": 140 * 1024}

WARNING_RE = re.compile(r"(?:^|/)(main/[^:\s]+):(\d+):\d+: warning: (.*?)(?: \[(-W[^\]]+)\])?$")

# Warnings accepted for now, each with its reason. None today.
KNOWN_WARNINGS = []

KEY_MATERIAL = [
    # The header alone is a string in the PEM parsers (mbedTLS); a key has a body
    (rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----(?:\\[nr]|\s)*[A-Za-z0-9+/]{40}",
     "PEM private key"),
    (rb"(?<![A-Za-z0-9_-])sk-(?:proj-|svcacct-|admin-)?[A-Za-z0-9_-]{32,}", "OpenAI-style API key"),
    (rb"(?<![A-Za-z0-9])AKIA[0-9A-Z]{16}(?![A-Za-z0-9])", "AWS access key id"),
    (rb"gh[pousr]_[A-Za-z0-9]{36}", "GitHub token"),
]

# Linked symbols that would open the attack surface AI_RULES.md keeps closed. The ESP32-C6
# firmware update (esp_hosted_slave_ota_*) is allowed: it sends the image embedded in the app.
FORBIDDEN_SYMBOLS = [
    (r"lwip_listen|lwip_accept|lwip_bind", "listening socket"),
    (r"httpd_\w+|esp_https_server\w*", "HTTP server"),
    (r"esp_https_ota\w*|esp_ota_begin|esp_ota_write", "OTA update of the radio"),
    (r"esp_bt_\w+|esp_ble_\w+|ble_hs_\w+|btc_\w+", "Bluetooth"),
    (r"mdns_\w+", "mDNS"),
    (r"dhcps_start|esp_netif_create_default_wifi_ap|esp_wifi_ap_get_sta_list", "WiFi access point"),
    (r"esp_now_\w+", "ESP-NOW"),
]
# (regular expression, what it is); the P4's hardware AES links GCM as esp_aes_gcm_*
REQUIRED_SYMBOLS = [
    (r"esp_crt_bundle_attach", "TLS verification with the CA bundle (directory, streams, sign-in pages)"),
    (r"esp_netif_create_default_wifi_sta", "WiFi station mode"),
    (r"esp_wifi_set_storage", "WiFi password kept out of the WiFi driver's flash"),
    (r"(?:esp_aes|mbedtls)_gcm_auth_decrypt", "the vault's AES-256-GCM"),
]

# Options that must hold in the generated sdkconfig (unset counts as "n")
REQUIRED_CONFIG = {
    "CONFIG_ESP_WIFI_SOFTAP_SUPPORT": "n",
    "CONFIG_LWIP_DHCPS": "n",
    "CONFIG_LWIP_IPV6": "n",
    "CONFIG_BT_ENABLED": "n",
    "CONFIG_ESP_WIFI_NVS_ENABLED": "n",
    "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE": "y",
    "CONFIG_ESP_TLS_INSECURE": "n",
    "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY": "n",
    # WiFi runs on the ESP32-C6 through esp_hosted / esp_wifi_remote, whose options matter too
    "CONFIG_WIFI_RMT_SOFTAP_SUPPORT": "n",
    "CONFIG_WIFI_RMT_NVS_ENABLED": "n",
    "CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE": "n",
    "CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID": "n",
    "CONFIG_ESP_WIFI_REMOTE_ENABLED": "y",
}


def error(message, file=None):
    where = f" file={file}" if file else ""
    print(f"::error{where}::{message}")


def summary(text):
    out = os.environ.get("GITHUB_STEP_SUMMARY")
    if out:
        with open(out, "a") as f:
            f.write(text + "\n")
    print(text)


def read_sdkconfig(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            m = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line)
            if m:
                values[m.group(1)] = m.group(2)
                continue
            m = re.match(r"^# (CONFIG_[A-Z0-9_]+) is not set$", line)
            if m:
                values[m.group(1)] = "n"
    return values


def sdkconfig_problems(values):
    return [f"{option}={values.get(option, 'n')} (must be {wanted})"
            for option, wanted in REQUIRED_CONFIG.items() if values.get(option, "n") != wanted]


def defined_symbols(nm_output):
    """Symbol names from `nm --defined-only` output."""
    names = set()
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            names.add(parts[-1])
    return names


def surface_problems(symbols, sdkconfig_values):
    problems = []
    for pattern, what in FORBIDDEN_SYMBOLS:
        hits = sorted(s for s in symbols if re.fullmatch(pattern, s))
        if hits:
            problems.append(f"{what} linked into the firmware: {', '.join(hits[:5])}")
    for pattern, what in REQUIRED_SYMBOLS:
        if not any(re.fullmatch(pattern, s) for s in symbols):
            problems.append(f"missing {pattern} ({what})")
    problems += [f"sdkconfig: {p}" for p in sdkconfig_problems(sdkconfig_values)]
    return problems


def key_material(data):
    return [what for pattern, what in KEY_MATERIAL if re.search(pattern, data)]


def warning_problems(log):
    """(blocking, known) lists of 'file:line: message' for warnings in main/."""
    blocking, known = [], []
    seen = set()
    for line in log.splitlines():
        m = WARNING_RE.search(line.strip())
        if not m or m.group(0) in seen:
            continue
        seen.add(m.group(0))
        path, row, message, _flag = m.groups()
        entry = f"{path}:{row}: {message}"
        (known if any(k.search(message) for k in KNOWN_WARNINGS) else blocking).append(entry)
    return blocking, known


def cmd_warnings(args):
    with open(args[0], encoding="utf-8", errors="replace") as f:
        blocking, known = warning_problems(f.read())
    for entry in blocking:
        path, row, message = entry.split(":", 2)
        error(f"compiler warning: {message.strip()}", f"{path},line={row}")
    summary(f"## Compiler warnings (main/)\n\n{len(blocking)} blocking, {len(known)} known.")
    return 1 if blocking else 0


def cmd_size(args):
    board = args[args.index("--board") + 1] if "--board" in args else "tab5"
    budget = BUDGETS[board]
    if "--budget" in args:
        budget = int(args[args.index("--budget") + 1])
    with open(args[0]) as f:
        size = json.load(f)
    used = size["used_diram"]
    lines = [f"## Firmware size ({board})", "",
             "| Region | Used | Limit |", "|---|---|---|",
             f"| Internal RAM, static (DIRAM) | {used:,} bytes | {budget:,} bytes |",
             f"| IRAM | {size.get('used_iram', 0):,} bytes | {size.get('iram_total', 0):,} bytes |",
             f"| Flash (code + rodata) | {size.get('used_flash_non_ram', 0):,} bytes | app partition |"]
    summary("\n".join(lines))
    if used > budget:
        error(f"Static internal RAM {used} bytes exceeds the {budget}-byte budget (AI_RULES.md, Memory and "
              "tasks): move buffers to PSRAM or explain the increase and raise the board's budget")
        return 1
    return 0


def cmd_secrets(args):
    failed = 0
    for path in args:
        with open(path, "rb") as f:
            found = key_material(f.read())
        for what in found:
            error(f"{what} found in {path}: nothing secret may be compiled into the firmware", path)
        failed += bool(found)
    if not failed:
        print(f"No key material in {len(args)} file(s)")
    return 1 if failed else 0


def cmd_surface(args):
    with open(args[0], encoding="utf-8", errors="replace") as f:
        symbols = defined_symbols(f.read())
    if len(symbols) < 1000:
        error(f"only {len(symbols)} symbols read from {args[0]}: is it nm output of the firmware ELF?")
        return 1
    problems = surface_problems(symbols, read_sdkconfig(args[1]))
    for p in problems:
        error(p)
    summary("## Network surface\n\n" + ("No problems: station-only WiFi, no listening sockets, servers, OTA "
                                       "or Bluetooth; TLS verification linked." if not problems else
                                       "\n".join(f"- {p}" for p in problems)))
    return 1 if problems else 0


def main():
    commands = {"size": cmd_size, "warnings": cmd_warnings, "secrets": cmd_secrets, "surface": cmd_surface}
    if len(sys.argv) < 3 or sys.argv[1] not in commands:
        print(__doc__)
        return 2
    return commands[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    sys.exit(main())
