# AI_RULES.md

Rules for anyone (people and AI agents) changing this repository. `CLAUDE.md` is a symbolic
link to this file and `AGENTS.md` points here: this is the one rules file. The guard tests in
`test/guards/` check the parts of these rules that can be checked in the source.

## What it is

SimplrRadio: an ESP32-P4 internet radio for the **M5Stack Tab5** (every panel revision, with or without the
Tab5 Keyboard) and the **Waveshare ESP32-P4-WIFI6-Touch-LCD-4B**. It streams MP3 stations from
the Radio Browser directory, shows the station, the song and a live spectrum, and is set up
entirely on its own screen: WiFi networks (scan, saved profiles, hidden networks, sign-in
pages of cafes and hotels) and the station search. It is a fork of David M. King's
esp32-p4-webradio (github.com/dmatking/esp32-p4-webradio, MIT; credit him in the README and
keep his copyright notices) and follows SimplrSSH's patterns
(github.com/MicroHealthLLC/SimplrSSH): the page UI, the encrypted vault, the settings
partition, the Tab5 bring-up, the launcher image and the CI pipelines.

## Scope and security

- **Station-only WiFi, IPv4.** No access point, no listening sockets, no servers, no
  Bluetooth, no mDNS, no OTA of the radio itself. The one firmware update is the ESP32-C6's,
  from the image embedded in the app (`slave_ota.c`).
- **Outbound only, verified TLS.** HTTPS everywhere with the ESP-IDF CA bundle
  (`crt_bundle_attach = esp_crt_bundle_attach`). Plain HTTP only for: the captive-portal
  probe (`connectivitycheck.gstatic.com/generate_204`), sign-in pages the network serves, and
  `http://` radio streams listed by the directory (audio only, nothing sent but the request).
- **Fixed endpoints:** `*.api.radio-browser.info` (station directory) and the probe. Stream
  URLs come from the directory and are accepted only as `http://` / `https://`.
- **No credentials in the firmware.** Nothing secret is compiled in; the CI scans the images.
- **WiFi passwords** live only in the vault (AES-256-GCM, `secret_vault.cpp`); the C6 is told
  to keep none (`esp_wifi_set_storage(WIFI_STORAGE_RAM)`, `CONFIG_*WIFI*_NVS_ENABLED=n`). A
  password entered on screen is wiped from RAM after use (`vault::wipe`, `prompt_wipe`).
- **A password means WPA2 or better** (`threshold.authmode = WIFI_AUTH_WPA2_PSK`): an impostor
  offering WEP/WPA1/open for a saved secured network is refused. PMF when the AP offers it.
- **Sign-in pages** (`portal*.cpp`): untrusted input, bounded (sign-in pages 192 KB in PSRAM /
  60 fields / 30 cookies), no scripts, cookies in RAM only, page state wiped when the sign-in
  ends. A page is checked for right after a connection the user made (or the boot join) and
  opened when the user asks; background reconnects never load one.
- **Text from the network** (station names, song titles, SSIDs, page text) reaches the screen
  only through `text_to_ascii()` / `portal::printable()`: printable ASCII, bounded.
- **Logging:** never log passwords, SSIDs typed by the user, page fields or cookies. Log
  formats are literals.

## Code layout

| File | What it does |
|---|---|
| `main/app.cpp` | Boot, screens (state machine), input, the worker task for blocking jobs |
| `main/ui_view.cpp` | LVGL drawing: title bar, pages, prompt + on-screen keyboard, Now Playing, spectrum |
| `main/ui_page.cpp` | Page / row model (host-tested, no LVGL) |
| `main/player.c` | Station list, debounced tuning, stream health, volume / mute |
| `main/stream.c` | HTTP(S) stream reader, ICY metadata, ring buffer |
| `main/mp3_decoder.c` | libhelix MP3 decode task to the codec and the spectrum |
| `main/spectrum.c` | FFT, 32 log-spaced bands with decay |
| `main/radio_browser.c` | Station directory query (mirrors, validation, text cleanup) |
| `main/wifi_mgr.cpp` | WiFi through the C6: start, scan, connect, saved networks, auto-connect |
| `main/portal.cpp` | Sign-in page parser (host-tested, from SimplrSSH) |
| `main/portal_client.cpp` | Sign-in page fetches (from SimplrSSH) |
| `main/secret_vault.cpp` | Encrypted WiFi passwords (host-tested) |
| `main/settings_nvs.cpp` | Which partition holds the settings (host-tested) |
| `main/settings.cpp` | Station search, volume, last station (host-tested) |
| `main/slave_ota.c` | ESP32-C6 firmware update from the embedded image |
| `main/text.c` | UTF-8 to screen ASCII (host-tested) |
| `main/board_m5stack_tab5.c` | Tab5: power, BSP display/touch (all panels), ES8388 audio, EQ, jack |
| `main/board_waveshare_p4_720.c` | Waveshare: ST7703 panel on LVGL, GT911 touch, ES8311 audio |
| `main/tab5_keyboard.c` | Tab5 Keyboard I2C driver (hot-plug) |
| `main/tab5_keymap.c` | Tab5 Keyboard key map (host-tested) |
| `main/lv_mem_psram.c` | LVGL's allocator: PSRAM first |

Board code sits behind `board.h` and `audio.h`; shared code has no pins, fonts or board
names. Fonts and sizes come from `board_ui()`.

## UI model

- Every menu and question is a `ui::Page`: numbered rows (type the number, Up/Down + Enter, or
  tap), Esc / the title bar's arrow goes back one level, Ctrl+Del (Tab5 Keyboard) returns to
  Now Playing. Questions are answered in a text field: the Tab5 Keyboard, or the on-screen
  keyboard when no keyboard is attached (always on the Waveshare).
- LVGL callbacks only post an event; the app task does the work, holding the display lock
  only while it changes the screen. Blocking work (WiFi, HTTP) runs in the worker task.
  Player calls that wait on tasks (stopping a stream) run after the lock is released.
- LVGL objects live in PSRAM (`CONFIG_LV_USE_CUSTOM_MALLOC`, `lv_mem_psram.c`).
- On-screen text is ASCII; sources are ASCII (icons are `LV_SYMBOL_*` escapes).

## Persistence

- Settings are NVS in the settings partition (`settings_nvs::open()` only): `"nvs"` **after
  the app** at `0x610000` (256 KB) when flashed at `0x0`, so a release image never erases it;
  the `"simplrradio"` partition when installed by a launcher (Bmorcelli Launcher / M5 Launcher),
  which keeps it across updates. Never move or shrink them, never erase a launcher's `"nvs"`.
- Namespaces: `radio` (search, volume, last station), `wifi` (saved networks), `vault`,
  `vault_dev` (the device secret).
- Volume and the last station are written only when they change (volume 2 s after the last
  step), not on every key press.

## Memory and tasks

- Bound every collection (saved networks 10, stations 60, station directory reply 128 KB in
  PSRAM, ICY block 4080 bytes, notices 1 KB, scan results 30).
- Tasks and stacks: `worker` 12 KB, `stream` 16 KB, `mp3dec` 16 KB, `hp_detect` 3 KB (Tab5),
  plus LVGL's (8 KB) and the main task (16 KB, runs the app). No other long-lived tasks.
- Big buffers go to PSRAM (`MALLOC_CAP_SPIRAM`).

## Dependencies

- Components are pinned with `==` in `main/idf_component.<board>.yml` and locked per board
  (`dependencies.lock.tab5`, `dependencies.lock.waveshare`, committed). Regenerate a lock with
  `scripts/update-lock.sh <board>`, never by committing what a clean configure rewrote: its
  second, Kconfig-aware pass writes local paths instead of registry hashes.
- Each board's full configuration is committed (`sdkconfig.tab5`, `sdkconfig.waveshare`), as in
  SimplrSSH; the defaults files say why each setting is there. Change a default, then rebuild
  and commit the board's sdkconfig.
- LVGL is pinned at 9.6.0~1: ESP-IDF 5.5.1's component manager lifts a 9.4.0 pin to it through
  esp_lvgl_port's dependency, and it is what SimplrSSH's Tab5 release builds run.
- esp_hosted stays at 2.12.3, matching the bundled C6 firmware (`slave_fw/network_adapter.bin`).
- ESP-IDF v5.5.1 (`espressif/idf` image pinned by digest in CI).

## Workflow

- Build: `BOARD=tab5 idf.py -B build-tab5 build` (or `BOARD=waveshare`, `build-waveshare`).
  Images: `idf.py -B build-<board> merge-bin -o <name>-release.bin` and `idf.py -B build-<board>
  launcher-bin`.
- Before pushing: `scripts/check.sh` (guard tests, host tests, SAST).
- Branches: work lands on `integration`; `main` is merged from it by pull request. The Tests
  workflow runs on every push; every push to `main` publishes a release (patch bump), and a
  manual run can publish a pre-release from `integration`.
- Hardware behaviour can only be confirmed on a real Tab5 / Waveshare: say so in pull
  requests that change board code.
