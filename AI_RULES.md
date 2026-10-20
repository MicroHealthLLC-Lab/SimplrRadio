# AI_RULES.md

Rules for anyone (people and AI agents) changing this repository. `CLAUDE.md` is a symbolic
link to this file and `AGENTS.md` points here: this is the one rules file. The guard tests in
`test/guards/` check the parts of these rules that can be checked in the source.

## What it is

SimplrRadio: an internet radio for two devices, the **M5Stack Tab5** (ESP32-P4, every panel revision,
with or without the Tab5 Keyboard) and the **LilyGO T-Deck / T-Deck Plus** (ESP32-S3, its keyboard
and trackball). No other boards are supported. It streams MP3 stations from
the Radio Browser directory, shows the station, the song and a live spectrum, and is set up
entirely on its own screen: WiFi networks (scan, saved profiles, hidden networks, sign-in
pages of cafes and hotels), the station search and Bluetooth LE keyboards and remotes. It is a fork of David M. King's
esp32-p4-webradio (github.com/dmatking/esp32-p4-webradio, MIT; credit him in the README and
keep his copyright notices) and follows SimplrSSH's patterns
(github.com/MicroHealthLLC/SimplrSSH): the page UI, the encrypted vault, the settings
partition, the Tab5 bring-up, the launcher image and the CI pipelines.

## Scope and security

- **Station-only WiFi, IPv4.** No access point, no listening sockets, no servers, no mDNS, no
  firmware updates over any link: not of the radio, not of the Tab5's ESP32-C6 (esp_hosted
  3.x works with the firmware it ships with, as in SimplrSSH).
- **Bluetooth LE central only** (`bt_hid.cpp`, NimBLE): a HID host for keyboards and remotes.
  It never advertises, has no GATT server and accepts no incoming connection; it scans only
  when the user asks and connects only to the device the user picks, or to devices the user
  paired. No Classic Bluetooth or Bluedroid (both radios are BLE only: the S3, and the Tab5's
  C6 through esp_hosted's HCI link). Off until the user turns it on. Pairing uses LE Secure
  Connections (legacy pairing only for keyboards without it), with a code shown on the screen
  for a keyboard to type. Bonding keys live only in the vault (`bt_store.cpp`); NimBLE keeps
  none in flash (`CONFIG_BT_NIMBLE_NVS_PERSIST=n`). Reports from a device are untrusted
  input: bounded and parsed by `hid_report.c` (host-tested).
- **Outbound only, verified TLS.** HTTPS everywhere with the ESP-IDF CA bundle
  (`crt_bundle_attach = esp_crt_bundle_attach`). Plain HTTP only for: the captive-portal
  probe (`connectivitycheck.gstatic.com/generate_204`), sign-in pages the network serves, and
  `http://` radio streams listed by the directory (audio only, nothing sent but the request).
- **Fixed endpoints:** `*.api.radio-browser.info` (station directory) and the probe. Stream
  URLs come from the directory and are accepted only as `http://` / `https://`.
- **No credentials in the firmware.** Nothing secret is compiled in; the CI scans the images.
- **WiFi passwords** live only in the vault (AES-256-GCM, `secret_vault.cpp`); the WiFi driver
  (on the Tab5, the C6) is told to keep none (`esp_wifi_set_storage(WIFI_STORAGE_RAM)`,
  `CONFIG_*WIFI*_NVS_ENABLED=n`). A
  password entered on screen is wiped from RAM after use (`vault::wipe`, `prompt_wipe`).
- **A password means WPA2 or better** (`threshold.authmode = WIFI_AUTH_WPA2_PSK`): an impostor
  offering WEP/WPA1/open for a saved secured network is refused. PMF when the AP offers it.
- **Sign-in pages** (`portal*.cpp`): untrusted input, bounded (sign-in pages 192 KB in PSRAM /
  60 fields / 30 cookies), no scripts, cookies in RAM only, page state wiped when the sign-in
  ends. A page is checked for right after a connection the user made (or the boot join) and
  opened when the user asks; background reconnects never load one.
- **Text from the network** (station names, song titles, SSIDs, page text, Bluetooth device
  names) reaches the screen only through `text_to_ascii()` / `portal::printable()`: printable
  ASCII, bounded.
- **Logging:** never log passwords, SSIDs typed by the user, page fields, cookies, Bluetooth
  keys, pairing codes or keys typed on a Bluetooth keyboard. Log formats are literals.

## Code layout

| File | What it does |
|---|---|
| `main/app.cpp` | Boot, screens (state machine), input, the worker task for blocking jobs |
| `main/ui_view.cpp` | LVGL drawing: title bar, pages, prompt + on-screen keyboard, Now Playing, spectrum |
| `main/ui_page.cpp` | Page / row model (host-tested, no LVGL) |
| `main/player.c` | Station list, debounced tuning, stream health, pause / play, volume / mute |
| `main/stream.c` | HTTP(S) stream reader, ICY metadata, ring buffer |
| `main/mp3_decoder.c` | libhelix MP3 decode task to the codec and the spectrum |
| `main/spectrum.c` | FFT, 32 log-spaced bands with decay |
| `main/radio_browser.c` | Station directory query (mirrors, validation, text cleanup) |
| `main/wifi_mgr.cpp` | WiFi: start, scan, connect, saved networks, auto-connect, power save |
| `main/bt_hid.cpp` | Bluetooth LE HID host (NimBLE central): scan, pair, reconnect, key reports |
| `main/bt_store.cpp` | Paired Bluetooth devices, Bluetooth on/off, bonding keys in the vault (host-tested) |
| `main/hid_report.c` | HID report map parser and input report decoder (host-tested) |
| `main/hid_keymap.c` | HID usages to the radio's keys, media keys included (host-tested) |
| `main/portal.cpp` | Sign-in page parser (host-tested, from SimplrSSH) |
| `main/portal_client.cpp` | Sign-in page fetches (from SimplrSSH) |
| `main/secret_vault.cpp` | Encrypted WiFi passwords and Bluetooth bonding keys (host-tested) |
| `main/settings_nvs.cpp` | Which partition holds the settings (host-tested) |
| `main/settings.cpp` | Station search, volume, last station (host-tested) |
| `main/text.c` | UTF-8 to screen ASCII (host-tested) |
| `main/board_m5stack_tab5.c` | Tab5: power, BSP display/touch (all panels), ES8388 audio, EQ, jack, C6 link (WiFi, Bluetooth) |
| `main/board_tdeck.c` | T-Deck: power, BSP ST7789 display, GT911 touch (polled, 0x5D or 0x14), I2S speaker with software volume |
| `main/tab5_keyboard.c` | Tab5 Keyboard I2C driver (hot-plug) |
| `main/tab5_keymap.c` | Tab5 Keyboard key map (host-tested) |
| `main/tdeck_keyboard.c` | T-Deck keyboard (I2C) and trackball |
| `main/tdeck_keymap.c` | T-Deck keyboard bytes and trackball press (host-tested) |
| `main/lv_mem_psram.c` | LVGL's allocator: PSRAM first |

Board code sits behind `board.h` and `audio.h`; shared code has no pins, fonts or board
names. Fonts, sizes and key names for hints come from `board_ui()`. The build picks the board
with `BOARD=tab5|tdeck`, and the board decides the chip (Tab5 `esp32p4`, T-Deck `esp32s3`).
Both follow SimplrSSH's board files: the same BSPs, display start, touch, power pins and, on the
Tab5, the same esp_hosted start (by the app once the C6 is powered, never before `app_main`).

## UI model

- Every menu and question is a `ui::Page`: numbered rows (type the number, Up/Down + Enter, the
  T-Deck's trackball, or tap), Esc / Left / the title bar's arrow goes back one level (T-Deck:
  hold the trackball), Right opens the highlighted row, Ctrl+Del (Tab5 Keyboard) returns to
  Now Playing. Questions are answered in a text field: the hardware keyboard, or the on-screen
  keyboard when none is attached.
- LVGL callbacks only post an event; the app task does the work, holding the display lock
  only while it changes the screen. Blocking work (WiFi, HTTP, Bluetooth start / stop / scan /
  pair) runs in the worker task. Player calls that wait on tasks (stopping a stream, pause,
  play) run after the lock is released.
- Now Playing has Pause / Play (button, Space or P, a Bluetooth Play/Pause key). A pause
  closes the stream and holds through WiFi drops and list reloads until Play or a station is
  picked. Media keys of Bluetooth keyboards and remotes work on every screen.
- LVGL objects live in PSRAM (`CONFIG_LV_USE_CUSTOM_MALLOC`, `lv_mem_psram.c`).
- On-screen text is ASCII; sources are ASCII (icons are `LV_SYMBOL_*` escapes).

## Persistence

- Settings are NVS in the settings partition (`settings_nvs::open()` only): `"nvs"` **after
  the app** at `0x610000` (256 KB) when flashed at `0x0`, so a release image never erases it;
  the `"simplrradio"` partition when installed by a launcher (Bmorcelli Launcher / M5 Launcher),
  which keeps it across updates. Never move or shrink them, never erase a launcher's `"nvs"`.
- Namespaces: `radio` (search, volume, last station), `wifi` (saved networks), `vault`,
  `vault_dev` (the device secret), `bt` (Bluetooth on/off, paired devices).
- Volume and the last station are written only when they change (volume 2 s after the last
  step), not on every key press.

## Memory and tasks

- Bound every collection (saved networks 10, stations 60, station directory reply 128 KB in
  PSRAM, ICY block 4080 bytes, notices 1 KB, scan results 30, paired Bluetooth devices 4,
  Bluetooth scan results 12, HID report map 512 bytes in PSRAM, HID report 64 bytes).
- Tasks and stacks: `worker` 12 KB, `stream` 16 KB, `mp3dec` 16 KB, `hp_detect` 3 KB (Tab5),
  plus LVGL's (8 KB), esp_hosted's (Tab5), the main task (16 KB, runs the app) and, while
  Bluetooth is on, NimBLE's host task (6 KB) and on the T-Deck the S3's controller task. No
  other long-lived tasks: the T-Deck's trackball is counted by interrupts and polled by the app.
- WiFi runs without modem sleep (streaming); while Bluetooth is on it uses modem sleep, which
  ESP-IDF's WiFi / Bluetooth coexistence needs (`wifi::set_power_save`).
- Big buffers go to PSRAM (`MALLOC_CAP_SPIRAM`).

## Dependencies

- Components are pinned with `==` in `main/idf_component.<board>.yml` and locked per board
  (`dependencies.lock.tab5`, `dependencies.lock.tdeck`, committed). Regenerate a lock with
  `scripts/update-lock.sh <board>`, never by committing what a clean configure rewrote: its
  second, Kconfig-aware pass writes local paths instead of registry hashes.
- Each board's full configuration is committed (`sdkconfig.tab5`, `sdkconfig.tdeck`), as in
  SimplrSSH; the defaults files say why each setting is there. Change a default, then rebuild
  and commit the board's sdkconfig.
- LVGL is pinned at 9.6.0~1 on both boards: ESP-IDF 5.5.1's component manager lifts a 9.4.0 pin
  to it in its second pass, and it is what SimplrSSH's release builds compile (their CI logs
  build `src/draw/nanovg`, which 9.4.0 doesn't have).
- The Tab5's WiFi stack is SimplrSSH's: esp_hosted 3.0.9 and esp_wifi_remote 1.6.5, with
  `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n` and the `ESP32P4_TAB5_C6_BOARD` preset.
  The T-Deck's display stack is SimplrSSH's: esp_bsp_generic 3.1.1, esp_lvgl_port 2.9.0, GT911.
- Bluetooth is ESP-IDF's own NimBLE (`bt` component), no extra managed component: on the Tab5
  esp_hosted's built-in HCI adapter (`CONFIG_ESP_HOSTED_HOST_FEAT_BT`,
  `esp_hosted_bt_host_stack_setup()` in the board file), on the T-Deck the S3's controller.
- ESP-IDF v5.5.1 (`espressif/idf` image pinned by digest in CI).

## Workflow

- Build: `BOARD=tab5 idf.py -B build-tab5 build` (or `BOARD=tdeck`, `build-tdeck`).
  Images: `idf.py -B build-<board> merge-bin -o <name>-release.bin` and `idf.py -B build-<board>
  launcher-bin`.
- Before pushing: `scripts/check.sh` (guard tests, host tests, SAST).
- Branches: work lands on `integration`; `main` is merged from it by pull request. The Tests
  workflow runs on every push; every push to `main` publishes a release (patch bump), and a
  manual run can publish a pre-release from `integration`.
- Hardware behaviour can only be confirmed on a real Tab5 / T-Deck: say so in pull
  requests that change board code.
