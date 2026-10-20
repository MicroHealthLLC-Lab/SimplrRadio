# ESP32-P4 Web Radio

A touchscreen internet radio for ESP32-P4 boards. It streams MP3 stations from the
[Radio Browser](https://www.radio-browser.info/) directory and shows the station, the song
playing and a live 32-band spectrum.

Everything is set up on the radio's own screen, with touch or the Tab5 Keyboard: pick your
WiFi network from a scan, sign in to cafe and hotel networks, choose which stations to load.
No phone, no setup network, nothing baked into the firmware.

## Supported boards

| Board | Display | Touch | Audio | Keyboard |
|-------|---------|-------|-------|----------|
| [M5Stack Tab5](https://docs.m5stack.com/en/products/sku/k145), every screen version | 1280x720 landscape (ILI9881C, ST7123 or ST7121, detected at boot) | GT911 / ST7123 | ES8388, speaker + headphone jack | Tab5 Keyboard (optional) |
| [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm) | 720x720 (ST7703) | GT911 | ES8311 | on-screen |

Both use an ESP32-C6 co-processor for WiFi. Each board has its own files; the other's won't
work.

## Install

Download the files for your board from the [latest release](../../releases/latest):
`WebRadio-Tab5-v<version>-...` or `WebRadio-Waveshare-v<version>-...`.

**With a launcher** ([Bmorcelli Launcher](https://github.com/bmorcelli/Launcher), M5 Launcher):

1. Copy `WebRadio-<board>-v<version>-launcher.bin` to the SD card.
2. In the launcher, open the SD card and install the file (or use its web installer).

Nothing is written at `0x0`: the launcher stays, installs the radio into a free slot and
creates the radio's own `webradio` settings partition, which it keeps when you install a newer
version the same way.

**Without a launcher**, flash from your browser (Chrome or Edge):

1. Connect the board over USB-C (Waveshare: the port labelled UART).
2. Open https://espressif.github.io/esptool-js/, click **Connect** and pick the board's port.
3. Add `WebRadio-<board>-v<version>-release.bin` at address **`0x0`** and click **Program**.
4. Press the reset button when it finishes.

For a Tab5, enter download mode by holding its reset button for about 2 seconds, until the
green LED flashes quickly. Or from a terminal:
`esptool.py --chip esp32p4 write_flash 0x0 WebRadio-<board>-v<version>-release.bin`.

**To update**, install the newer file the same way. Your WiFi networks and settings are kept.
Avoid "Erase flash".

**The first start takes about 30 seconds**: the radio brings its WiFi chip's firmware up to
date (it travels inside the radio's firmware). Later starts skip it.

## First start: WiFi

The radio shows the WiFi networks around it, strongest first. Pick yours (tap it, or type its
number), type the password and press Enter (on the Tab5 Keyboard, or on the
on-screen keyboard; **Tab** or the **Show** button reveals what you typed). The network is
saved, and the radio connects to the strongest saved network in range at every start.

- **Hidden network:** choose *Hidden network...* at the end of the list and type its name.
- **Cafe / hotel sign-in pages:** right after connecting, the radio checks whether the network
  wants you to sign in, and if so shows its sign-in page as a form: tick the boxes, fill in
  the fields, press its buttons. You can open it again any time from *Menu > Sign in to this
  network*. Scripts don't run, so pages that need them won't work; a phone hotspot does.
- If the WiFi drops, the radio reconnects by itself and resumes playing.

Saved passwords are stored encrypted on the radio (AES-256-GCM). WiFi networks can be added,
switched, forgotten or disconnected in *Menu > WiFi*.

## Controls

**Now Playing**, by touch: the buttons along the bottom (Menu, previous, mute, next, volume
down / up, Stations); swipe left / right for the next / previous station, up / down for
volume.

**With the Tab5 Keyboard:**

| Key | Action |
|-----|--------|
| Left / Right | Previous / next station |
| Up / Down | Volume up / down |
| Space or M | Mute / unmute |
| A number, then Enter | Go to that station |
| Enter or S | Station list |
| W | WiFi |
| Esc | Menu |
| Ctrl+Del | Back to Now Playing from anywhere |

**In menus:** Up / Down and Enter, or type a row's number (it opens as soon as no longer
number could match; otherwise press Enter), or tap it. Esc (or the arrow in the
title bar) goes back. Sym+Up / Sym+Down scrolls a long page.

**Menu:** Stations, WiFi, Station search (country, state / region, how many stations), Sign
in to this network, About (versions, WiFi, memory).

The radio remembers the last station and the volume. A station that refuses to play (some
block listeners outside their region) shows "unavailable": just go to the next one. A station
that drops is retried, then skipped.

## Building from source

See **[BUILDING.md](BUILDING.md)**: toolchain, per-board builds, release and launcher
images, tests and the CI pipelines.

## License

Apache-2.0. The sign-in page parser, the vault, the page UI and the Tab5 Keyboard support
come from [SimplrSSH](https://github.com/MicroHealthLLC/SimplrSSH) (MIT).
