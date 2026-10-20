# SimplrRadio

An internet radio for the **M5Stack Tab5** (with or without its keyboard) and the
**Waveshare ESP32-P4 4" touch board**. Join WiFi on the screen, pick from popular stations
near you, and listen, with the station, the song and a live spectrum on the display.

> SimplrRadio is a fork of [**esp32-p4-webradio**](https://github.com/dmatking/esp32-p4-webradio)
> by [David M. King (@dmatking)](https://github.com/dmatking). Thanks to him for the original
> radio: the streaming, MP3 decoding, spectrum analyzer, the Waveshare and Tab5 bring-up, and
> the ESP32-C6 firmware update. It is released under the same MIT license (see
> [LICENSE](LICENSE)). Its menus, WiFi setup, sign-in pages, encrypted passwords and CI come
> from [**SimplrSSH**](https://github.com/MicroHealthLLC/SimplrSSH). Full credits are
> [below](#credits).

## Supported devices

- **M5Stack Tab5**, every screen version (the radio detects which one), with or without the
  **Tab5 Keyboard**: files named `SimplrRadio-Tab5-v<version>-...`. The Tab5 runs it in
  landscape, on the full 1280x720 screen.
- **Waveshare ESP32-P4-WIFI6-Touch-LCD-4B** (720x720): files named
  `SimplrRadio-Waveshare-v<version>-...`. Text is typed on the on-screen keyboard.

Each device has its own files; the other's won't start. No other boards are supported. You
also need a USB-C cable to install, and a 2.4 GHz WiFi network with internet access.

SimplrRadio is new and has not been tested on a Tab5 or a Waveshare board yet: please report
what you find.

## Install

Download the latest firmware from the [**Releases**](../../releases) page.

**If your device runs a launcher** (such as [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher),
the M5 Launcher):

1. Copy `SimplrRadio-Tab5-v<version>-launcher.bin` (Waveshare:
   `SimplrRadio-Waveshare-v<version>-launcher.bin`) to the SD card.
2. In the launcher, open the SD card and install the file.

Nothing is written at `0x0`: the launcher stays, and keeps the radio's settings when you
install a newer version the same way.

**Otherwise, flash from your browser** (Chrome or Edge):

1. Connect the device over USB (Waveshare: the port labelled UART).
2. Open https://espressif.github.io/esptool-js/, click **Connect** and pick the device's port.
3. Add `SimplrRadio-Tab5-v<version>-release.bin` (Waveshare:
   `SimplrRadio-Waveshare-v<version>-release.bin`) at address **`0x0`** and click **Program**.
4. Press the reset button when it finishes.

For a Tab5, hold its reset button for about 2 seconds, until the green LED flashes quickly,
to put it in download mode.

**To update**, install the newer file the same way. Your settings are kept. Avoid "Erase flash".

**The first start takes about 30 seconds**: the radio updates the firmware of its WiFi chip
(an ESP32-C6, which usually ships with old firmware). Later starts skip it.

**Coming from esp32-p4-webradio 1.x?** Its setup network (`P4-Radio-Setup`) is gone: set
WiFi up on the screen as below. Settings from 1.x aren't carried over.

## Configure

The radio starts by joining WiFi, then plays. The bar at the top shows the page's title and,
on the right, the volume and a WiFi icon (red while WiFi is off). Every menu is a list of rows:

- **Tab5 Keyboard**: Up / Down to move, Enter to open.
- **Touch**: tap a row. Drag to scroll a long page.
- **Both**: type a row's number (it opens as soon as no longer number could match; otherwise
  press Enter).

To go back one level, pick **0 Back**, tap the arrow in the title bar or press **Esc**;
**Ctrl+Del** returns to Now Playing from anywhere. While a step works (scanning, connecting,
loading stations) its progress shows on the page. When a step asks a question, type the
answer into the field under it and press Enter (or the on-screen keyboard's check mark).

1. **WiFi**: on the first start the radio shows the networks around it, strongest first. Pick
   yours and type the password (**Tab** or **Show** reveals what you typed). It is saved and
   the radio plays. From then on it joins the strongest saved network in range by itself,
   after a restart too, and reconnects if WiFi drops. *Hidden network...* at the end of the
   list takes a network's name. If the network has a sign-in page (cafe, hotel, airport), see
   *Sign-in pages* below.
2. **Stations**: the radio loads the most popular MP3 stations for a region from the
   [Radio Browser](https://www.radio-browser.info/) directory, and resumes the last one you
   played. Choose **Menu** → **3 Station search** to pick the **country** (two letters, empty
   for worldwide), the **state / region** (empty for the whole country) and **how many
   stations** (up to 60), then **Load stations now**. The default is the 60 most popular in
   Texas, US.

The **Menu**: **1 Stations**, **2 WiFi** (scan and connect, saved networks, forget,
disconnect, sign in), **3 Station search**, **4 Sign in to this network** and **5 About**
(versions, WiFi, memory).

Saved WiFi passwords are encrypted on the device.

## Use

- **Now Playing** shows the station, the song (when the station sends it), the station's
  number, bitrate and genre, and a live spectrum. The buttons along the bottom: **Menu**,
  previous, mute, next, volume down / up, **Stations**.
- **Change station**: the previous / next buttons, swipe left / right, Left / Right on the
  keyboard, or pick one from **Stations**. Type a station's number and Enter to jump to it.
  Paging quickly through stations only starts the one you stop on.
- **Volume**: the buttons, swipe up / down, or Up / Down on the keyboard. Mute with the button
  or Space / M. The volume and the last station are remembered.
- **Headphones** (Tab5): plugging them in turns the speaker off and adds a little bass;
  unplugging turns it back on. Mute silences both.
- **A station that won't play** (some only stream to their own region) shows *unavailable*:
  go to the next one. A station that drops is retried, then skipped.
- **Sign-in pages (captive portals)**: in a cafe or hotel, the WiFi may connect but let
  nothing through until you accept its terms on a web page. Right after you connect, the radio
  checks, and if the network wants a sign-in it shows the page as a form: its text on top,
  then a row for each thing on it. Pick a row to tick a box, choose an option, fill in a field,
  press a button (such as *Connect*) or follow a link. **Start again** reloads and checks the
  connection, **0 Back** leaves. Once it is online the radio plays. You can open it any time
  with **Menu** → **4 Sign in to this network**. Nothing from the page is saved, and pages
  that need JavaScript can't work - accept on your phone and use its hotspot instead.

### Tab5 keyboard

| Keys | Does |
|---|---|
| Left / Right | Previous / next station (in a text field: move the cursor) |
| Up / Down | Volume up / down; move the highlight in a menu |
| Space or M | Mute / unmute |
| A number, then Enter | Go to that station (in a menu: open that row) |
| Enter or S | Station list |
| W | WiFi |
| Esc | Menu; back one level in a menu |
| Ctrl+Del | Back to Now Playing from any screen |
| Tab | Show / hide a password while typing it |
| Sym+Up / Sym+Down | Scroll a long page |
| Sym+Left / Sym+Right | Start / end of the line in a text field |
| Aa | Hold for capitals; tap for one capital; tap twice for caps lock |
| Sym + key | The key's second symbol (`?` `/` `<` `>` `{` `}` `\|` `~` `:` `"` `=` `,`) |

Held keys repeat. The keyboard can be attached or removed while the radio runs; without it,
the on-screen keyboard appears when there's something to type.

## Troubleshooting

- **WiFi won't connect**: make sure the network is 2.4 GHz and shows up in the scan. The
  message says why: *Wrong password?*, *Network not found*, or *old security (WEP/WPA)*: the
  radio only joins secured networks that use WPA2 or newer.
- **WiFi connects but no stations load** (cafe, hotel): the network wants you to sign in
  first. Use **Menu** → **4 Sign in to this network**. *Nothing to fill in or press here*: the
  page needs JavaScript; sign in on a phone and use its hotspot.
- **"The station list couldn't be loaded"**: no internet yet, or the station directory is
  busy; the radio tries again by itself, waiting longer each time.
- **The WiFi chip isn't responding** at start: choose **Try again**, or restart the radio.
  The first start after installing takes about 30 seconds while the WiFi chip updates.
- **Black screen on a Tab5**: this version supports every Tab5 screen; if yours stays dark,
  please report it with the serial log (115200 baud).

## Credits

- **[David M. King (@dmatking)](https://github.com/dmatking)** wrote
  [esp32-p4-webradio](https://github.com/dmatking/esp32-p4-webradio), which this is a fork of:
  the HTTP/ICY streaming, the MP3 decode path, the 32-band spectrum analyzer, the
  ES8311/ES8388 audio with the Tab5's EQ and headphone detection, the Waveshare board
  bring-up, the Radio Browser client and the ESP32-C6 firmware update over esp_hosted.
  MIT license.
- **[SimplrSSH](https://github.com/MicroHealthLLC/SimplrSSH)** (MicroHealth, MIT), itself a
  fork of [PocketSSH](https://github.com/0015/PocketSSH) by [Eric (@0015)](https://github.com/0015):
  the page UI, the sign-in page (captive portal) parser and client, the encrypted vault, the
  settings partition for launchers, the Tab5 Keyboard driver and key map, the Tab5 power-up,
  and the test and CI setup.
- **[Radio Browser](https://www.radio-browser.info/)**: the free, community-run station
  directory. Please consider supporting it.
- **[Espressif](https://github.com/espressif)**: ESP-IDF, esp_hosted and esp_wifi_remote,
  the `m5stack_tab5` board support package (which tells the Tab5's screens apart),
  esp_lvgl_port, esp-dsp, esp_codec_dev and the ST7703 panel driver in `components/`
  (Apache-2.0).
- **[LVGL](https://lvgl.io/)** (MIT): the graphics library behind every screen.
- **[esp-libhelix-mp3](https://github.com/chmorgan/esp-libhelix-mp3)** by Chris Morgan: the
  Helix MP3 decoder (RealNetworks) packaged for ESP-IDF.
- **[M5Stack](https://m5stack.com/)**: the Tab5, its keyboard, and the keyboard firmware's
  register protocol.
- **[bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)**: the launcher the
  `-launcher.bin` images are made for.

## More

- Security details and how to report a vulnerability are in [SECURITY.md](SECURITY.md).
- To build it yourself you need ESP-IDF v5.5.1: `BOARD=tab5 idf.py -B build-tab5 build`
  (or `BOARD=waveshare`, `build-waveshare`), then
  `idf.py -B build-tab5 merge-bin -o SimplrRadio-Tab5-v<version>-release.bin` and
  `idf.py -B build-tab5 launcher-bin`. Details in [BUILDING.md](BUILDING.md).
- Every push and pull request runs the **Tests** checks (`.github/workflows/tests.yml`). To run
  the unit tests on a PC (Linux, with `cmake`, `g++` and `libmbedtls-dev`):
  `cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host`,
  and `python3 -m unittest discover -s test/guards`. `scripts/check.sh` runs both, then
  `scripts/sast.sh` (MicroHealth's SAST rules; needs docker or nerdctl logged in to
  MicroHealth's ECR; `SKIP_SAST=1` leaves it out).
- Every push to `main` publishes a release (**Build firmware**, `.github/workflows/build.yml`).
- The **Security scan** workflow (`.github/workflows/security-scan.yml`) runs SAST, Trivy, an
  SBOM and ZAP DAST and writes one report; see [SECURITY.md](SECURITY.md).
- Contributor rules are in [AI_RULES.md](AI_RULES.md).
