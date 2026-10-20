#!/usr/bin/env python3
"""
Build GitHub release notes for a firmware version (after SimplrSSH's release_notes.py).

Uses a "### v<version>" section from README.md when there is one (optional); otherwise the
notes point to the change list GitHub generates from the commits and pull requests since the
previous release, which the workflow appends below. Adds install instructions and the image
checksums for both boards.

Usage: release_notes.py <version> <tab5-image> <tab5-sha256> <tab5-launcher> <tab5-launcher-sha256>
                        <waveshare-image> <waveshare-sha256> <waveshare-launcher> <waveshare-launcher-sha256>
                        <output.md>
"""

import re
import sys


def version_section(readme, version):
    """Lines under '### v<version> ...' up to the next '### ' or '## ' heading."""
    lines = readme.splitlines()
    heading = re.compile(r"^###\s+v?" + re.escape(version) + r"(\s|$)")
    for i, line in enumerate(lines):
        if heading.match(line):
            body = []
            for next_line in lines[i + 1:]:
                if next_line.startswith("### ") or next_line.startswith("## "):
                    break
                body.append(next_line)
            return lines[i][4:].strip(), "\n".join(body).strip()
    return None, None


def board_section(title, image, sha256, launcher_image, launcher_sha256, download_mode):
    return f"""
## {title}

**With a launcher (Bmorcelli Launcher / M5 Launcher):** copy **`{launcher_image}`** to the SD
card and install it from the launcher's SD menu (or its web installer). The launcher puts the
app in a free slot and creates the radio's own `simplrradio` settings partition, which it keeps
when you install a newer version the same way. Nothing is written at `0x0`.

SHA-256: `{launcher_sha256}` (also in `{launcher_image}.sha256`)

**Without a launcher:** flash **`{image}`** at offset **`0x0`** (ESP32-P4, 16 MB). {download_mode}

- **Browser**: open https://espressif.github.io/esptool-js/, connect the board over USB-C,
  add `{image}` at address `0x0`, and click *Program*.
- **Command line**:
  ```
  esptool.py --chip esp32p4 --baud 921600 write_flash 0x0 {image}
  ```

SHA-256: `{sha256}` (also in `{image}.sha256`; check with `sha256sum -c {image}.sha256`)
"""


def build_notes(readme, version, tab5, waveshare):
    """Release notes text; tab5 / waveshare = (image, sha256, launcher_image, launcher_sha256)."""
    title, changes = version_section(readme, version)
    if changes:
        whats_new = f"## What's new in {title}\n\n{changes}\n"
    else:
        whats_new = (f"## SimplrRadio v{version}\n\n"
                     "The changes in this release are listed under *What's Changed* below.\n")
    notes = whats_new + """
Setup happens on the radio's screen: pick your WiFi network (touch or the Tab5 Keyboard),
sign in to a cafe or hotel network if it asks, and choose which stations to load. Your
settings are kept across updates. The first start after installing updates the WiFi chip
(about 30 seconds).
"""
    notes += board_section("M5Stack Tab5", *tab5,
                           download_mode="To enter download mode, hold the Tab5's reset button for about "
                                         "2 seconds, until the green LED flashes quickly.")
    notes += board_section("Waveshare ESP32-P4-WIFI6-Touch-LCD-4B", *waveshare,
                           download_mode="Use the port labelled UART.")
    return notes


def main():
    args = sys.argv[1:]
    if len(args) != 10:
        print(__doc__)
        return 2
    version = args[0]
    tab5 = tuple(args[1:5])
    waveshare = tuple(args[5:9])
    output = args[9]
    with open("README.md", encoding="utf-8") as f:
        notes = build_notes(f.read(), version, tab5, waveshare)
    with open(output, "w", encoding="utf-8") as f:
        f.write(notes)
    print(notes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
