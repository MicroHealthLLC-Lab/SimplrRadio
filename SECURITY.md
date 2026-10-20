# Security

## Reporting a vulnerability

Please report security issues privately through GitHub's *Report a vulnerability* (Security
tab) rather than in a public issue. Include the board, the firmware version (Menu > About)
and how to reproduce it.

## What the radio does on the network

- **Station-only WiFi, IPv4.** No access point, no DHCP server, no listening sockets, no
  servers, no Bluetooth, no mDNS. The radio never accepts a connection.
- **Outbound connections only:** the Radio Browser directory (HTTPS, certificates verified
  with ESP-IDF's CA bundle), the radio streams it lists (HTTP or HTTPS, audio only), the
  captive-portal probe `http://connectivitycheck.gstatic.com/generate_204` and the sign-in
  pages a network serves.
- **No firmware updates over the network.** The only update is the ESP32-C6 WiFi chip's, from
  the image inside the radio's own firmware.

## Secrets

- No credentials or keys are compiled in; CI scans every image for key material.
- **WiFi passwords** are stored encrypted with AES-256-GCM (`main/secret_vault.cpp`). The data
  key is wrapped under a random per-device secret; each record is bound to its network by
  authenticated data. The WiFi chip is told to keep no copy (`WIFI_STORAGE_RAM`), and
  passwords typed on screen are wiped from memory after use.
- A secured network is only joined with WPA2 or better (an impostor offering WEP, WPA or an
  open network under the same name is refused); management frames are protected when the
  access point supports it.
- Sign-in pages are untrusted input: size-bounded, no scripts, cookies in RAM only, wiped when
  the sign-in ends. Nothing typed into a page is saved.

## Changes since 1.x

1.x set WiFi up through an open setup network (`P4-Radio-Setup`) with a web form served by the
radio, and kept the password in plain text. 2.0 removed the access point and the web server
entirely; setup happens on the screen.

## Checks

The Tests workflow audits ESP-IDF and every component against the NVD (reviewed exceptions in
`.github/audit-baseline.txt`), verifies the linked firmware has no servers, sockets, OTA or
Bluetooth, scans commits for secrets (gitleaks), and pins every action, image and component.
MicroHealth's shared security scan (SAST, Trivy, SBOM, ZAP) runs on `main`, `integration` and
pull requests.
