# Building from source

This covers building and flashing the firmware yourself. To just run it, install a release
(see the [README](README.md)).

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.5.1** with the ESP32-P4
  toolchain, or Docker with the same image CI uses:
  `espressif/idf:v5.5.1@sha256:dfa2d076c796769c07c155eba6c672b9f395aec943b2ba3701b73379b5f9e884`.
- One of the supported boards (ESP32-P4 v1.x silicon + ESP32-C6 WiFi co-processor).

## Selecting a board

The board comes from the `BOARD` environment variable (`tab5`, the default, or `waveshare`).
It selects the board source files, the component manifest (`main/idf_component.<board>.yml`),
the component lock (`dependencies.lock.<board>`) and the configuration (`sdkconfig.defaults`
+ `sdkconfig.defaults.<board>`, applied to the committed `sdkconfig.<board>`). Use one build
directory per board:

```bash
BOARD=tab5 idf.py -B build-tab5 build
BOARD=waveshare idf.py -B build-waveshare build
```

## Flash

```bash
BOARD=tab5 idf.py -B build-tab5 -p /dev/ttyACM0 flash monitor
```

The ESP32-C6 firmware (`slave_fw/network_adapter.bin`, esp_hosted 2.12.3) is embedded in the
app: `idf.py flash` brings it along, and the first start updates the C6 if needed.

## Release images

```bash
BOARD=tab5 idf.py -B build-tab5 merge-bin -o SimplrRadio-Tab5-v<ver>-release.bin   # flash at 0x0
BOARD=tab5 idf.py -B build-tab5 launcher-bin   # build-tab5/SimplrRadio-Tab5-v<ver>-launcher.bin
```

The launcher image uses `partitions_launcher.csv`: the app at `0x50000` and, before it, the
`simplrradio` settings partition (SPIFFS subtype, which launchers create and keep). The version
comes from `FIRMWARE_VERSION` (CI) or `PROJECT_VER` in `CMakeLists.txt`.

## Partitions and settings

| Image | Settings | Where |
|---|---|---|
| Flashed at `0x0` (`partitions.csv`) | `nvs`, 256 KB at `0x610000` | after the 6 MB app, so re-flashing never erases it |
| Launcher (`partitions_launcher.csv`) | `simplrradio`, 256 KB | the launcher keeps it across updates |

`main/settings_nvs.cpp` picks the partition at boot. To forget every network and setting on a
directly flashed radio: `esptool.py --chip esp32p4 erase_region 0x610000 0x40000`.

## Tests

```bash
scripts/check.sh            # guard tests, host unit tests (cmake, g++, libmbedtls-dev), SAST
SKIP_SAST=1 scripts/check.sh
```

- `test/host/`: the hardware-independent firmware code (sign-in page parser, page model, Tab5
  key map, text cleanup, vault, settings, settings partition) built for Linux with ASan/UBSan.
- `test/guards/`: Python checks that keep [AI_RULES.md](AI_RULES.md) true (supply chain,
  partitions, network surface, firmware rules, CI scripts).

## Dependencies

Components are pinned with `==` per board and locked in `dependencies.lock.<board>`. After
changing a manifest, regenerate the board's lock and configuration with
`scripts/update-lock.sh <board>` (Docker) and commit both. Plain `idf.py update-dependencies`
from a clean tree writes local paths into the lock (a second, Kconfig-aware resolve pass);
the script keeps the registry entries the guards require.

## CI (GitHub Actions)

The same pipelines as SimplrSSH, on `main` and `integration`:

| Workflow | Runs on | What |
|---|---|---|
| `tests.yml` | every push, pull requests | The gate: install, lint, typecheck, unit tests, persistence, build both boards, CVE audit, network surface, secret scan |
| `build.yml` | `main`, `integration`, tags, pull requests | Builds both boards' release and launcher images; **every push to `main` publishes a release** (patch bump) once Tests passed; a manual run can release from `integration` as a pre-release |
| `code-quality.yml` | `main`, `integration`, pull requests | cppcheck, flawfinder, compiler warnings (report only) |
| `dependency-check.yml` | weekly, lock changes | Outdated components and ESP-IDF (report only) |
| `security-scan.yml` | `main`, `integration`, pull requests, daily | MicroHealth's shared scan: SAST, Trivy, SBOM, ZAP |

Work lands on `integration`; `main` is updated from it by pull request. Make "Tests" a
required check on both branches.

The release job tags the commit it built. GitHub doesn't let a workflow token tag a commit whose
workflow files differ from the ones on `main`, so if a pull request that changes
`.github/workflows/` (a Dependabot action bump) is merged while a release is still running,
that release is refused; the release for the newer `main` commit then publishes the version.
Merge workflow updates once a release has finished.

## Architecture

```
main/app.cpp            boot, screens, input; worker task for blocking jobs
main/ui_view.cpp        LVGL: title bar, pages, prompts + on-screen keyboard, Now Playing, spectrum
main/ui_page.cpp        page / row model
main/player.c           stations, debounced tuning, stream health, volume / mute
main/stream.c           HTTP(S) stream + ICY metadata into a ring buffer
main/mp3_decoder.c      libhelix MP3 decode to the codec and the spectrum
main/spectrum.c         512-point FFT (esp-dsp), 32 log-spaced bands
main/radio_browser.c    station directory (HTTPS, mirrors)
main/wifi_mgr.cpp       WiFi via the ESP32-C6, saved networks, auto-connect
main/portal*.cpp        captive-portal sign-in pages
main/secret_vault.cpp   AES-256-GCM WiFi passwords
main/settings*.cpp      settings partition, radio settings
main/slave_ota.c        ESP32-C6 firmware update from the embedded image
main/board_m5stack_tab5.c     Tab5: BSP display/touch (all panels), ES8388, EQ, headphone jack
main/board_waveshare_p4_720.c Waveshare: ST7703 on LVGL, GT911, ES8311
main/tab5_keyboard.c, tab5_keymap.c   Tab5 Keyboard
components/esp_lcd_st7703     Waveshare panel driver (vendored)
```

`board.h` / `audio.h` are the interfaces each board file implements; fonts and sizes come from
`board_ui()`.

### Notes

- **Tab5 display:** the panel came in three versions; the `espressif/m5stack_tab5` BSP tells
  them apart by probing the touch controller. LVGL renders landscape and the PPA rotates it.
- **Tab5 power:** the IO expanders' outputs are set push-pull (after the driver's reset every
  pin is high-impedance), powering the WiFi chip, the speaker amp and the keyboard's 5 V.
- **Streaming** sends a VLC `User-Agent` (some stations require it).
- **Console:** UART primary + USB-JTAG secondary.
