# Security

## Reporting a vulnerability

Please report security issues privately through GitHub's *Report a vulnerability* (Security
tab) rather than in a public issue. Include the board, the firmware version (Menu > About)
and how to reproduce it.

## What the radio does on the network

- **Station-only WiFi, IPv4.** No access point, no DHCP server, no listening sockets, no
  servers, no mDNS. The radio never accepts a connection.
- **Bluetooth LE, as a central only** (keyboards and remotes, `main/bt_hid.cpp`, NimBLE). Off
  until the user turns it on. The radio never advertises, offers no GATT services and accepts
  no incoming connection: it scans only when asked and connects to the device the user picks
  or has paired. No Classic Bluetooth or Bluedroid. Pairing uses LE Secure Connections (legacy
  pairing only for keyboards without it; a keyboard types a random 6-digit code shown on the
  screen). Key reports are untrusted input: the report map and every report are bounded and
  parsed by host-tested code (`main/hid_report.c`); device names are cleaned to printable
  ASCII before they are shown.
- **Outbound connections only:** the Radio Browser directory (HTTPS, certificates verified
  with ESP-IDF's CA bundle), the radio streams it lists (HTTP or HTTPS, audio only), the
  captive-portal probe `http://connectivitycheck.gstatic.com/generate_204` and the sign-in
  pages a network serves.
- **No firmware updates of any kind.** Nothing is flashed over the network, and the Tab5's
  ESP32-C6 WiFi chip keeps the firmware it shipped with.

## Secrets

- No credentials or keys are compiled in; CI scans every image for key material.
- **WiFi passwords** are stored encrypted with AES-256-GCM (`main/secret_vault.cpp`). The data
  key is wrapped under a random per-device secret; each record is bound to its network by
  authenticated data. The WiFi driver is told to keep no copy (`WIFI_STORAGE_RAM`), and
  passwords typed on screen are wiped from memory after use.
- A secured network is only joined with WPA2 or better (an impostor offering WEP, WPA or an
  open network under the same name is refused); management frames are protected when the
  access point supports it.
- Sign-in pages are untrusted input: size-bounded, no scripts, cookies in RAM only, wiped when
  the sign-in ends. Nothing typed into a page is saved.
- **Bluetooth bonding keys** are kept in the same encrypted vault as the WiFi passwords
  (`main/bt_store.cpp`); NimBLE keeps no copy in flash (`CONFIG_BT_NIMBLE_NVS_PERSIST=n`).
  Forgetting a device deletes its keys. Pairing codes and typed keys are never logged.

## Changes since 1.x

1.x set WiFi up through an open setup network (`P4-Radio-Setup`) with a web form served by the
radio, and kept the password in plain text. 2.0 removed the access point and the web server
entirely; setup happens on the screen.

## Repository settings

The repository is public: anyone can read, clone and fork it, and nothing more. Its GitHub
settings (not in the tree; the same as SimplrSSH's; change them only on purpose): issues,
wiki, discussions and projects are off; only collaborators can open pull requests; workflows
from an outside contributor's fork wait for approval; the default workflow token is read-only
and cannot approve pull requests (each workflow declares what it writes); a ruleset stops
deletion and force-pushes of `main` and `integration` (repository admins bypass it); secret
scanning with push protection, Dependabot alerts and private vulnerability reporting are on.

**Secrets stay here.** The security scan needs `secrets.AWS_ROLE` (an OIDC role, no stored AWS
keys) on this repository, or shared with it by the organization (an org secret limited to
private repositories never reaches a public one), and the role's OIDC trust must include
`repo:MicroHealthLLC/SimplrRadio`, so a fork's workflows can't assume it. A fork never
receives this repository's secrets or variables, and GitHub doesn't pass secrets to workflows
run for pull requests from forks; no workflow uses `pull_request_target`
(test/guards/test_supply_chain.py), and outside contributors' fork workflows need approval.

## Checks

The Tests workflow audits ESP-IDF and every component against the NVD (reviewed exceptions in
`.github/audit-baseline.txt`), verifies the linked firmware has no servers, sockets or OTA, and
no Classic Bluetooth, Bluetooth advertising, GATT server or NimBLE key store (Bluetooth LE
central only), scans commits for secrets (gitleaks), and pins every action, image and component.
MicroHealth's shared security scan (SAST, Trivy, SBOM, ZAP) runs on `main`, `integration` and
pull requests.

### SAST review (2026-10-20)

The security scan reported 41 Opengrep findings (3 were in `slave_ota.c`, removed with the C6
firmware update). The other 38 were checked by hand; the same rules and Opengrep now report 0:

| Finding | Outcome |
|---|---|
| `atoi` on typed station and menu numbers, and on the station count (no overflow detection) | One helper: three digits at most, `strtol` with an end check, 0 to 999, else rejected |
| `snprintf` with formats from macros (the muted label, `IPSTR`) | A literal label; `esp_ip4addr_ntoa` |
| Unbounded `strlen` on a directory URL, a stream URL, the SSID being joined, and a literal | `strnlen` with the destination's limit; `sizeof` on the literal |
| `memcpy` of the SSID and password into the WiFi config | Clamped to the config's fields as well as checked against 32 / 63 characters before |
| `memset` (can be optimized away when clearing secrets) on spectrum, key map and EQ filter state | None held a secret; replaced by a struct assignment and loops, or dropped (statics start at zero). Secrets are cleared with `mbedtls_platform_zeroize` |
| 21 more: `memcpy` between buffers of the same fixed size or with the bound checked on the line before, `strlen` on strings the firmware NUL-terminates | Not applicable: suppressed inline with the full rule id and the bound |

### SAST review (2026-10-21)

The scan of the Bluetooth LE code reported 16 Opengrep findings. None could overflow or leak; the
code was changed so the patterns are gone, and the same rules and Opengrep now report 0:

| Finding | Outcome |
|---|---|
| `memcpy` of 6-byte Bluetooth addresses (`bt_hid.cpp`, 6 findings) | Every source and destination is `uint8_t[6]`. Now one helper that takes both as array references, so the compiler checks the sizes |
| `memcpy` of bonding keys into and out of the vault record (`bt_hid.cpp`, 4 findings) | The record's length was checked before reading. Now built with `std::string::append` and read with `std::string::copy`, which check the bounds |
| `memcpy` of an advertised device name (`bt_hid.cpp`) | Clamped to the buffer the line before. Now a `std::string` of at most 31 bytes |
| `memset` on the HID report map, fields, local items and decoded input (`hid_report.c`, 5 findings) | None held a secret, and each is read right after, so it can't be optimized away. Replaced by struct assignments |
