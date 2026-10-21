# SimplrRadio

An internet radio and music player for the **M5Stack Tab5** (with or without its keyboard) and the
**LilyGO T-Deck**. Join WiFi on the screen, pick from popular stations near you, and listen,
with the station, the song and a live spectrum on the display. Or play your own music from a
microSD card (MP3, AAC, M4A, ALAC, FLAC, Ogg Vorbis, Opus, WAV, AMR), sorted by artist, album
and genre. A Bluetooth keyboard or media remote can work it too.

> SimplrRadio is a fork of [**esp32-p4-webradio**](https://github.com/dmatking/esp32-p4-webradio)
> by [David M. King (@dmatking)](https://github.com/dmatking). Thanks to him for the original
> radio: the streaming, MP3 decoding, spectrum analyzer and the first Tab5 bring-up. It is
> released under the same MIT license (see [LICENSE](LICENSE)). Its menus, WiFi setup, sign-in
> pages, encrypted passwords, T-Deck and Tab5 board support and CI come from
> [**SimplrSSH**](https://github.com/MicroHealthLLC/SimplrSSH). Full credits are
> [below](#credits).

## Supported devices

- **M5Stack Tab5**, every screen version (the radio detects which one), with or without the
  **Tab5 Keyboard**: files named `SimplrRadio-Tab5-v<version>-...`. The Tab5 runs it in
  landscape, on the full 1280x720 screen.
- **LilyGO T-Deck / T-Deck Plus** (320x240 touch screen, keyboard and trackball): files named
  `SimplrRadio-TDeck-v<version>-...`. The sound comes from its built-in speaker.

Each device has its own files; the other's won't start. No other boards are supported. You
also need a USB-C cable to install, and a 2.4 GHz WiFi network with internet access.

SimplrRadio is new: please report what you find on your device.

## Install

Download the latest firmware from the [**Releases**](../../releases) page.

**If your device runs a launcher** (such as [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher),
the M5 Launcher):

1. Copy `SimplrRadio-Tab5-v<version>-launcher.bin` (T-Deck:
   `SimplrRadio-TDeck-v<version>-launcher.bin`) to the SD card.
2. In the launcher, open the SD card and install the file.

Nothing is written at `0x0`: the launcher stays, and keeps the radio's settings when you
install a newer version the same way.

**Otherwise, flash from your browser** (Chrome or Edge):

1. Connect the device over USB-C.
2. Open https://espressif.github.io/esptool-js/, click **Connect** and pick the device's port.
3. Add `SimplrRadio-Tab5-v<version>-release.bin` (T-Deck:
   `SimplrRadio-TDeck-v<version>-release.bin`) at address **`0x0`** and click **Program**.
4. Restart the device when it finishes.

To put it in download mode: on a Tab5, hold its reset button for about 2 seconds, until the
green LED flashes quickly; on a T-Deck, hold the trackball down while switching it on.

**To update**, install the newer file the same way. Your settings are kept. Avoid "Erase flash".

**Coming from esp32-p4-webradio 1.x?** Its setup network (`P4-Radio-Setup`) is gone: set
WiFi up on the screen as below. Settings from 1.x aren't carried over.

## Configure

The radio starts by joining WiFi, then plays. The bar at the top shows the page's title and,
on the right, the volume, a Bluetooth icon while a Bluetooth keyboard or remote is connected,
and a WiFi icon (red while WiFi is off). Every menu is a list of rows:

- **Tab5 Keyboard**: Up / Down to move, Enter (or Right) to open.
- **T-Deck**: roll the trackball up / down to move, press it (or roll right) to open.
- **Touch**: tap a row. Drag to scroll a long page.
- **Keyboards**: type a row's number (it opens as soon as no longer number could match;
  otherwise press Enter).

To go back one level, pick **0 Back**, tap the arrow in the title bar, roll left, press
**Esc** (Tab5) or **hold the trackball** (T-Deck); **Ctrl+Del** (Tab5) returns to Now Playing
from anywhere. While a step works (scanning, connecting, loading stations) its progress shows
on the page. When a step asks a question, type the answer into the field under it and press
Enter (or the on-screen keyboard's check mark).

1. **WiFi**: on the first start the radio shows the networks around it, strongest first. Pick
   yours and type the password (**Show**, or **Tab** on the Tab5, reveals it). It is saved and
   the radio plays. From then on it joins the strongest saved network in range by itself,
   after a restart too, and reconnects if WiFi drops. *Hidden network...* at the end of the
   list takes a network's name. If the network has a sign-in page (cafe, hotel, airport), see
   *Sign-in pages* below.
2. **Stations**: the radio loads the most popular MP3 stations for a region from the
   [Radio Browser](https://www.radio-browser.info/) directory, and resumes the last one you
   played. Choose **Menu** → **4 Station search** to pick the **country** (two letters, empty
   for worldwide), the **state / region** (empty for the whole country) and **how many
   stations** (up to 60), then **Load stations now**. The default is the 60 most popular in
   Texas, US.

The **Menu**: **1 Stations**, **2 Music** (songs on the SD card), **3 WiFi** (scan and connect,
saved networks, forget, disconnect, sign in), **4 Station search**, **5 Sign in to this
network**, **6 Bluetooth** (keyboards and remotes) and **7 About** (versions, WiFi, Bluetooth,
music, memory).

Saved WiFi passwords and Bluetooth pairing keys are encrypted on the device.

## Use

- **Now Playing** shows the station, the song (when the station sends it), the station's
  number, bitrate and genre, and a live spectrum. The buttons along the bottom: **Menu**,
  previous, **pause / play**, next, mute, volume down / up, **Stations**.
- **Stop the radio**: the pause button, Space or P (or Play/Pause on a Bluetooth remote)
  stops the stream; it stays stopped, even if WiFi drops and comes back, until you press play
  again or pick a station.
- **Change station**: the previous / next buttons, swipe left / right, Left / Right on the
  keyboard or trackball, or pick one from **Stations**. Type a station's number and Enter to
  jump to it. Paging quickly through stations only starts the one you stop on.
- **Volume**: the buttons, swipe up / down, or Up / Down on the keyboard or trackball. Mute
  with the button or M. The volume and the last station are remembered.
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
  with **Menu** → **5 Sign in to this network**. Nothing from the page is saved, and pages
  that need JavaScript can't work - accept on your phone and use its hotspot instead.

### Music from the SD card

Put your music on a microSD card (FAT32), in any folders, and slide it into the device: the
Tab5's card slot, or the T-Deck's.

1. **Menu** → **2 Music**. The first time, the radio reads the whole card and lists every song
   it can play: MP3, AAC, M4A (AAC or Apple Lossless), FLAC, Ogg Vorbis, Opus, WAV and AMR.
   To use one folder only, choose **8 Settings** → **1 Music folder**, open folders until you
   are in yours and pick **Use this folder**.
2. Each song's **title, artist, album, genre, year and track number** come from its tags
   (ID3, FLAC / Ogg comments, MP4 / iTunes tags, WAV info). When a file has none, they come
   from its name and folders: `Artist/Album/03 - Title.mp3`, `Artist - Album/Title.flac` or
   `Artist - Title.ogg` (a `CD1` / `Disc 2` folder is skipped), and names typed in all capitals
   or all lower case get their capitals fixed.
3. **Genres** missing from your files are looked up on the internet, artist by artist, in the
   [MusicBrainz](https://musicbrainz.org/) music database, while the radio is on WiFi. Only the
   artist's name is sent. Turn it off with **Settings** → **5 Look up missing genres online**.
4. Pick **1 Songs**, **2 Artists**, **3 Albums**, **4 Genres**, **5 Favorites**, **6 Search**
   (words from the title, artist or album) or **7 Shuffle all songs**. In a list of songs,
   **Play all** and **Shuffle all** play the whole list, or pick a song to start there. Long
   lists come in pages of 40: **Next page** / **Previous page**.
5. **Now Playing** is the same as for the radio: the song's title, its artist and album, its
   number in the list, format, genre and time, and the spectrum. Previous / next, pause / play,
   volume, mute, swipes and Bluetooth media keys work on the songs; the list button opens the
   songs playing. A pause keeps your place in the song, and songs keep playing without WiFi.
6. **Favorites**: while a song plays, press **F** on Now Playing, or choose **Add the song
   playing to favorites** in the Music page (up to 256).
7. **Settings**: the music folder, **Scan the folder again** (after you add or remove songs),
   **Shuffle** on / off, **Repeat** (off, the list, this song) and the online genres.

Pick a station to go back to the radio. Whichever played last (a station or a song) plays
again when the device starts. The library is kept on the card in a `.simplrradio` folder, so
it isn't read again at every start; nothing else on the card is changed. Up to 3,000 songs and
8 folders deep. Not playable: M4A files whose index comes after the audio (re-save them with
"fast start"), 32-bit float WAV, sample rates above 96 kHz; a song that can't be played is
skipped. Seeking within a song isn't available.

### Bluetooth keyboards and remotes

Both devices connect to **Bluetooth LE** keyboards, media remotes and page-turner clickers.
Their keys work the radio: Play/Pause, next / previous, volume and mute on every screen, and
arrows, Enter, Esc and typing in the menus (WiFi passwords too).

1. **Menu** → **6 Bluetooth** → **1 Bluetooth** turns it on (it stays on after a restart).
2. **2 Pair a keyboard or remote**, put the device in pairing mode, and pick it from the
   list. A keyboard may be asked to type a 6-digit code shown on the screen, then Enter.
3. From then on it connects by itself whenever it is switched on near the radio. Pick it in
   the Bluetooth page to forget it. Up to four devices are remembered, one is connected at a
   time.

Both radios are Bluetooth LE only (the T-Deck's ESP32-S3, and the Tab5's ESP32-C6): **Bluetooth
headphones and speakers can't connect**, as they need Classic Bluetooth. While Bluetooth is on,
WiFi shares the antenna with it. On the Tab5, Bluetooth needs the C6's own firmware to include
its Bluetooth controller; if it doesn't, the page says Bluetooth didn't start.

### Tab5 keyboard

| Keys | Does |
|---|---|
| Left / Right | Previous / next station or song (in a text field: move the cursor) |
| Up / Down | Volume up / down; move the highlight in a menu |
| Space or P | Pause / play |
| M | Mute / unmute |
| F | Add the song playing to favorites (or remove it) |
| A number, then Enter | Go to that station or song (in a menu: open that row) |
| Enter or S | Station list (while music plays: the songs playing) |
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

### T-Deck keyboard and trackball

| Input | Does |
|---|---|
| Trackball left / right | Previous / next station or song; back a level / open a row in a menu (in a text field: move the cursor) |
| Trackball up / down | Volume up / down; move the highlight in a menu |
| Press the trackball | Station list (while music plays: the songs playing); open the highlighted row; Enter in a text field |
| Hold the trackball | Menu; back one level (the T-Deck's Esc) |
| Space or P | Pause / play |
| M | Mute / unmute |
| F | Add the song playing to favorites (or remove it) |
| A number, then Enter | Go to that station or song (in a menu: open that row) |
| S / W | Station list / WiFi |
| Backspace | Delete a character; back one level in a menu |

Passwords are shown with the **Show** button next to the field.

## Troubleshooting

- **WiFi won't connect**: make sure the network is 2.4 GHz and shows up in the scan. The
  message says why: *Wrong password?*, *Network not found*, or *old security (WEP/WPA)*: the
  radio only joins secured networks that use WPA2 or newer. The *(reason N)* at the end is
  ESP-IDF's code for it; include it when you report a problem.
- **T-Deck touch**: the touch panel answers at one of two addresses depending on how it
  powered up; the radio looks for both. If taps still do nothing, please report it with the
  serial log (it says *Touch panel at 0x5D* or *0x14*, or *Touch panel not found*).
- **Bluetooth device won't pair**: put it in pairing mode again, and if it was paired with
  the radio before, forget it on both sides first. Keyboards type the code shown, then Enter.
- **WiFi connects but no stations load** (cafe, hotel): the network wants you to sign in
  first. Use **Menu** → **5 Sign in to this network**. *Nothing to fill in or press here*: the
  page needs JavaScript; sign in on a phone and use its hotspot.
- **"The station list couldn't be loaded"**: no internet yet, or the station directory is
  busy; the radio tries again by itself, waiting longer each time.
- **"WiFi didn't start"**: choose **Try again**, or restart the radio.
- **"No SD card found"**: the card must be FAT32. Cards of 64 GB and more come formatted
  exFAT, which the radio can't read: format it FAT32 on a computer (this erases it). Then
  **Music** → **Settings** → **Scan the folder again**.
- **Songs missing from the library**: only the formats above are read; a path longer than
  about 190 characters or more than 8 folders deep is left out (the scan says how many). After
  adding songs, choose **Scan the folder again**.
- **Black screen**: the Tab5 starts its screen and WiFi the way SimplrSSH's Tab5 firmware does, on every
  Tab5 screen version. If yours stays dark, please report it with the serial log (USB-C,
  115200 baud).

## Credits

- **[David M. King (@dmatking)](https://github.com/dmatking)** wrote
  [esp32-p4-webradio](https://github.com/dmatking/esp32-p4-webradio), which this is a fork of:
  the HTTP/ICY streaming, the MP3 decode path, the 32-band spectrum analyzer, the ES8388
  audio with the Tab5's EQ and headphone detection, the first Tab5 bring-up and the Radio
  Browser client. MIT license.
- **[SimplrSSH](https://github.com/MicroHealthLLC/SimplrSSH)** (MicroHealth, MIT), itself a
  fork of [PocketSSH](https://github.com/0015/PocketSSH) by [Eric (@0015)](https://github.com/0015):
  the page UI, the sign-in page (captive portal) parser and client, the encrypted vault, the
  settings partition for launchers, the Tab5 Keyboard driver and key map, the Tab5 power-up
  and WiFi co-processor link, the T-Deck board support (display, touch, keyboard, trackball),
  and the test and CI setup.
- **[Radio Browser](https://www.radio-browser.info/)**: the free, community-run station
  directory. Please consider supporting it.
- **[Espressif](https://github.com/espressif)**: ESP-IDF (with NimBLE), esp_hosted and esp_wifi_remote,
  the `m5stack_tab5` board support package (which tells the Tab5's screens apart),
  `esp_bsp_generic` (the T-Deck's display), esp_lvgl_port, esp-dsp and esp_codec_dev
  (Apache-2.0).
- **[LVGL](https://lvgl.io/)** (MIT): the graphics library behind every screen.
- **[esp-libhelix-mp3](https://github.com/chmorgan/esp-libhelix-mp3)** by Chris Morgan: the
  Helix MP3 decoder (RealNetworks) packaged for ESP-IDF.
- **[esp_audio_codec](https://components.espressif.com/components/espressif/esp_audio_codec)**
  by Espressif ([esp-adf-libs](https://github.com/espressif/esp-adf-libs), Espressif Modified
  MIT: for Espressif chips): every music file decoder (MP3, AAC / HE-AAC, ALAC, FLAC, Vorbis,
  Opus, PCM / ADPCM / G.711 WAV, AMR) and the container parsers (M4A, Ogg, WAV), the same ones
  [ESP-ADF](https://github.com/espressif/esp-adf)'s music players use. The music player plays
  SD cards the way ESP-ADF's `music_player` example does, with its decode loop.
- **[MusicBrainz](https://musicbrainz.org/)** (MetaBrainz Foundation): the open music
  database the missing genres come from. Its core data is CC0; its tags and genres are
  CC BY-NC-SA 3.0. Please consider supporting it.
- The ID3 genre list is the ID3v1 list with Winamp's extensions, as every tag reader knows it.
  GPL players (ESP32-audioI2S, arduino-audio-tools, ESP8266Audio) were looked at, not used: the
  radio's tag reader is its own, small and bounded for files nobody checked.
- **[M5Stack](https://m5stack.com/)**: the Tab5, its keyboard, and the keyboard firmware's
  register protocol.
- **[LilyGO](https://lilygo.cc/)**: the T-Deck and its pin map.
- **[bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)**: the launcher the
  `-launcher.bin` images are made for.

## More

- Security details and how to report a vulnerability are in [SECURITY.md](SECURITY.md).
- To build it yourself you need ESP-IDF v5.5.1: `BOARD=tab5 idf.py -B build-tab5 build`
  (or `BOARD=tdeck`, `build-tdeck`), then
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
