"""
Firmware rules from AI_RULES.md that can be checked in the source: the code layout, settings
access, task stacks, bounds, logging hygiene, secrets, board separation and the UI model.
(After SimplrSSH's test_firmware_rules.py.)
"""

import glob
import os
import re
import unittest

import repo

SRC = {name: repo.strip_comments(text) for name, text in repo.sources().items()}
C_SOURCES = ("*.c", "*.cpp")


def present_sources():
    return {os.path.basename(f) for pattern in C_SOURCES for f in glob.glob(os.path.join(repo.MAIN, pattern))}


def grep(pattern, exclude=()):
    hits = []
    for name, code in SRC.items():
        if os.path.basename(name) in exclude:
            continue
        for i, line in enumerate(code.splitlines(), 1):
            if re.search(pattern, line):
                hits.append(f"{name}:{i}: {line.strip()}")
    return hits


BOARD_FILES = {
    "tab5": {"board_m5stack_tab5.c", "tab5_keyboard.c", "tab5_keymap.c"},
    "tdeck": {"board_tdeck.c", "tdeck_keyboard.c", "tdeck_keymap.c"},
}


class Build(unittest.TestCase):
    def test_cmake_lists_every_source(self):
        cmake = repo.read("main", "CMakeLists.txt")
        listed = set(re.findall(r'"([\w]+\.c(?:pp)?)"', cmake))
        self.assertEqual(listed, present_sources(), "main/CMakeLists.txt must list every main/*.c and *.cpp")

    def test_board_sources_listed_per_board(self):
        cmake = repo.read("main", "CMakeLists.txt")
        tab5, rest = cmake.split('if(BOARD STREQUAL "tab5")', 1)[1].split("else()", 1)
        tdeck = rest.split("endif()", 1)[0]
        self.assertEqual(set(re.findall(r'"(\w+\.c)"', tab5)), BOARD_FILES["tab5"])
        self.assertEqual(set(re.findall(r'"(\w+\.c)"', tdeck)), BOARD_FILES["tdeck"])

    def test_only_the_two_boards(self):
        cmake = repo.read("CMakeLists.txt")
        self.assertIn('BOARD STREQUAL "tab5"', cmake)
        self.assertIn('BOARD STREQUAL "tdeck"', cmake)
        self.assertIn('set(BOARD_TARGET "esp32p4")', cmake)
        self.assertIn('set(BOARD_TARGET "esp32s3")', cmake)
        boards = sorted(os.path.basename(f) for f in glob.glob(os.path.join(repo.MAIN, "board_*.c")))
        self.assertEqual(boards, ["board_m5stack_tab5.c", "board_tdeck.c"])
        for board in ("tab5", "tdeck"):
            for name in (f"sdkconfig.defaults.{board}", f"sdkconfig.{board}", f"dependencies.lock.{board}",
                         f"main/idf_component.{board}.yml"):
                with self.subTest(file=name):
                    self.assertTrue(os.path.isfile(repo.path(*name.split("/"))))

    def test_ai_rules_code_layout_covers_every_source(self):
        layout = repo.read("AI_RULES.md").split("## Code layout", 1)[1].split("\n## ", 1)[0]
        documented = set(re.findall(r"`main/([\w]+\.c(?:pp)?)`", layout))
        present = present_sources()
        self.assertEqual(sorted(present - documented), [], "add new source files to the Code layout table")
        self.assertEqual(sorted(documented - present), [], "Code layout lists files that no longer exist")

    def test_no_firmware_updates(self):
        # As SimplrSSH: nothing is ever flashed over a link, the C6 included (esp_hosted 3.x works
        # with the firmware it ships with)
        self.assertFalse(os.path.exists(repo.path("slave_fw")))
        self.assertEqual(grep(r"esp_hosted_slave_ota|esp_ota_begin|esp_https_ota"), [])


class Settings(unittest.TestCase):
    def test_namespaces_opened_through_settings_nvs(self):
        self.assertEqual(grep(r"\bnvs_open\s*\(", exclude=("settings_nvs.cpp",)), [],
                         "open namespaces with settings_nvs::open()")
        self.assertEqual(grep(r"\bnvs_open_from_partition\s*\(", exclude=("settings_nvs.cpp",)), [])
        self.assertEqual(grep(r"NVS_DEFAULT_PART_NAME", exclude=("settings_nvs.cpp",)), [],
                         "use settings_nvs::partition()")

    def test_launcher_nvs_never_erased(self):
        self.assertEqual(grep(r"\bnvs_flash_erase\s*\(\s*\)"), [], "never erase the default nvs")
        self.assertEqual(grep(r"nvs_flash_erase_partition(_ptr)?\s*\(", exclude=("settings_nvs.cpp",)), [])
        self.assertEqual(grep(r"\bnvs_flash_init\s*\(\s*\)"), [], "the settings partition is chosen by settings_nvs")

    def test_namespaces_are_the_documented_ones(self):
        used = set()
        for code in SRC.values():
            used |= set(re.findall(r'settings_nvs::open\(\s*"(\w+)"', code))
            used |= set(re.findall(r'static const char\s*\*\s*(?:NS|NS_\w+)\s*=\s*"(\w+)"', code))
        self.assertEqual(used, {"radio", "wifi", "vault", "vault_dev", "bt", "music"})
        rules = repo.read("AI_RULES.md")
        for ns in used:
            with self.subTest(namespace=ns):
                self.assertIn(f"`{ns}`", rules)

    def test_volume_saved_after_a_pause(self):
        player = SRC["main/player.c"]
        self.assertIn("VOLUME_SAVE_MS", player)
        set_volume = player.split("void player_set_volume", 1)[1].split("\n}", 1)[0]
        self.assertNotIn("settings_set_volume", set_volume, "save volume from player_tick, not per step")


class Tasks(unittest.TestCase):
    # AI_RULES.md, Memory and tasks
    STACKS = {"worker": 12288, "stream": "TASK_STACK", "mp3dec": "TASK_STACK", "filedec": "TASK_STACK",
              "hp_detect": 3072}

    def test_task_stacks_match_budget(self):
        found = {}
        for code in SRC.values():
            for name, stack in re.findall(r'(?:xTaskCreate\w*|task_stack_start)\(\s*\w+\s*,\s*"(\w+)"\s*,\s*(\w+)', code):
                found[name] = int(stack) if stack.isdigit() else stack
        self.assertEqual(found, self.STACKS, "a new or resized task needs AI_RULES.md updated")
        self.assertIn("#define TASK_STACK      (16 * 1024)", repo.read("main", "stream.c"))
        self.assertIn("#define TASK_STACK      (16 * 1024)", repo.read("main", "mp3_decoder.c"))
        self.assertIn("#define TASK_STACK      (16 * 1024)", repo.read("main", "music_decoder.c"))

    def test_no_busy_wait(self):
        self.assertEqual(grep(r"while\s*\(\s*(1|true)\s*\)\s*;"), [])

    def test_main_task_stack(self):
        self.assertIn("CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384", repo.read("sdkconfig.defaults").splitlines())


class Bounds(unittest.TestCase):
    def test_collection_bounds_documented(self):
        rules = re.sub(r"\s+", " ", repo.read("AI_RULES.md"))
        saved = int(re.search(r"MAX_SAVED = (\d+);", SRC["main/wifi_mgr.hpp"]).group(1))
        stations = int(re.search(r"#define MAX_STATIONS\s+(\d+)", SRC["main/radio_browser.h"]).group(1))
        reply_kb = int(re.search(r"RESP_BUF_SIZE \((\d+) \* 1024\)", repo.read("main", "radio_browser.c")).group(1))
        scan = int(re.search(r"SCAN_MAX = (\d+);", SRC["main/wifi_mgr.cpp"]).group(1))
        bounds = re.search(r"Bound every collection \(([^)]*)\)", rules).group(1)
        self.assertIn(f"saved networks {saved}", bounds)
        self.assertIn(f"stations {stations}", bounds)
        self.assertIn(f"station directory reply {reply_kb} KB in PSRAM", bounds)
        self.assertIn(f"scan results {scan}", bounds)

    def test_bluetooth_bounds(self):
        rules = re.sub(r"\s+", " ", repo.read("AI_RULES.md"))
        devices = int(re.search(r"MAX_DEVICES = (\d+);", SRC["main/bt_store.hpp"]).group(1))
        found = int(re.search(r"MAX_FOUND = (\d+);", SRC["main/bt_hid.hpp"]).group(1))
        map_bytes = int(re.search(r"#define HID_MAX_MAP_BYTES\s+(\d+)", SRC["main/hid_report.h"]).group(1))
        bounds = re.search(r"Bound every collection \(([^)]*)\)", rules).group(1)
        self.assertIn(f"paired Bluetooth devices {devices}", bounds)
        self.assertIn(f"Bluetooth scan results {found}", bounds)
        self.assertIn(f"HID report map {map_bytes} bytes", bounds)
        sdk = repo.read("sdkconfig.defaults")
        self.assertIn(f"CONFIG_BT_NIMBLE_MAX_BONDS={devices}", sdk)

    def test_music_bounds(self):
        rules = re.sub(r"\s+", " ", repo.read("AI_RULES.md"))
        bounds = re.search(r"Bound every collection \(([^)]*)\)", rules).group(1)
        lib = SRC["main/music_library.hpp"]
        tags = repo.read("main", "media_tags.h")
        tracks = int(re.search(r"MAX_TRACKS = (\d+);", lib).group(1))
        path = int(re.search(r"PATH_LEN = (\d+);", lib).group(1))
        depth = int(re.search(r"MAX_FOLDER_DEPTH = (\d+);", lib).group(1))
        favorites = int(re.search(r"MAX_FAVORITES = (\d+);", lib).group(1))
        cache = int(re.search(r"MAX_GENRE_CACHE = (\d+);", lib).group(1))
        folders = int(re.search(r"MAX_FOLDERS = (\d+);", SRC["main/music_player.h"]).group(1))
        reply_kb = int(re.search(r"REPLY_MAX\s+\((\d+) \* 1024\)", repo.read("main", "music_online.c")).group(1))
        text = int(re.search(r"MEDIA_TEXT_MAX\s+(\d+)", tags).group(1))
        comments_kb = int(re.search(r"MEDIA_COMMENTS_MAX\s+\((\d+) \* 1024\)", tags).group(1))
        frames_kb = int(re.search(r"OUT_MAX\s+\((\d+) \* 1024\)", repo.read("main", "music_decoder.c")).group(1))
        for wanted in (f"music library {tracks} songs in PSRAM", f"song paths {path} bytes",
                       f"music folders {depth} deep and {folders} per listing", f"favorites {favorites}",
                       f"online genres {cache} artists", f"MusicBrainz reply {reply_kb} KB in PSRAM",
                       f"tag values {text} bytes", f"comment blocks {comments_kb} KB",
                       f"decoded music frames {frames_kb} KB in PSRAM"):
            with self.subTest(bound=wanted):
                self.assertIn(wanted, bounds)
        self.assertIn("MALLOC_CAP_SPIRAM", SRC["main/music_player.cpp"])   # big_alloc: the library in PSRAM

    def test_sd_card_never_formatted(self):
        for board in ("board_m5stack_tab5.c", "board_tdeck.c"):
            with self.subTest(board=board):
                self.assertIn(".format_if_mount_failed = false", SRC[f"main/{board}"])
        self.assertEqual(grep(r"esp_vfs_fat_sdcard_format|f_mkfs"), [])

    def test_sign_in_page_bounds(self):
        header = repo.read("main", "portal.hpp")
        rules = re.sub(r"\s+", " ", repo.read("AI_RULES.md"))
        page_kb = int(re.search(r"MAX_PAGE_BYTES = (\d+) \* 1024;", header).group(1))
        fields = int(re.search(r"MAX_FIELDS = (\d+);", header).group(1))
        cookies = int(re.search(r"MAX_COOKIES = (\d+);", header).group(1))
        self.assertIn(f"sign-in pages {page_kb} KB in PSRAM / {fields} fields / {cookies} cookies", rules)
        self.assertIn("MALLOC_CAP_SPIRAM", SRC["main/portal_client.cpp"])

    def test_icy_metadata_bounded(self):
        stream = SRC["main/stream.c"]
        self.assertIn("ICY_META_MAX     (255 * 16)", repo.read("main", "stream.c"))
        self.assertIn("ICY_MAX_METAINT", stream)
        self.assertIn("v <= ICY_MAX_METAINT", stream)


class Logging(unittest.TestCase):
    LOG = re.compile(r"ESP_LOG[EWIDV]\s*\(\s*(\w+)\s*,\s*([^;]*)\)\s*;")
    SENSITIVE = re.compile(r"\b\w*(password|passwd|passphrase|secret|api_?key|private_?key|"
                           r"cookie|ssid|pass|plain|prompt_text|field)\w*\b", re.I)

    def test_log_formats_are_literals(self):
        for name, code in SRC.items():
            for _tag, args in self.LOG.findall(code):
                with self.subTest(file=name, args=args[:60]):
                    self.assertTrue(args.lstrip().startswith('"'), "no format strings built from data")

    def test_no_secrets_or_user_input_logged(self):
        for name, code in SRC.items():
            for _tag, args in self.LOG.findall(code):
                values = args.split('"')[-1] if '"' in args else args   # Arguments after the format
                values = re.sub(r"[\w.]+\.(size|empty|length)\(\)", "", values)   # Counts are fine
                for word in self.SENSITIVE.findall(values):
                    with self.subTest(file=name, log=args[:80]):
                        self.fail(f"logs '{word}': never log passwords, SSIDs, page fields or cookies")


class Secrets(unittest.TestCase):
    def test_wifi_passwords_only_in_the_vault(self):
        mgr = SRC["main/wifi_mgr.cpp"]
        self.assertIn("vault::put(secret_id(ssid), password)", mgr)
        self.assertIn("esp_wifi_set_storage(WIFI_STORAGE_RAM)", mgr)
        store = mgr.split("static esp_err_t store_saved()", 1)[1].split("\n}", 1)[0]
        self.assertNotRegex(store, r"\bpassword\b", "saved networks hold no password")

    def test_secrets_wiped(self):
        self.assertIn("mbedtls_platform_zeroize", SRC["main/secret_vault.cpp"])
        self.assertIn("mbedtls_platform_zeroize", SRC["main/ui_view.cpp"])
        self.assertIn("mbedtls_platform_zeroize(&cfg, sizeof(cfg))", SRC["main/wifi_mgr.cpp"])
        self.assertGreaterEqual(sum(code.count("vault::wipe(") for code in SRC.values()), 4)

    def test_wpa2_floor_for_secured_networks(self):
        self.assertIn("password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK", SRC["main/wifi_mgr.cpp"])
        self.assertIn("pmf_cfg.capable = true", SRC["main/wifi_mgr.cpp"])

    def test_no_credentials_in_the_source(self):
        self.assertEqual(grep(r'(?i)\b(?:password|passwd|psk)\s*=\s*"[^"]+"'), [])
        self.assertEqual(grep(r"CONFIG_WIFI_(?:SSID|PASSWORD)"), [], "no compiled-in WiFi credentials")


class Boards(unittest.TestCase):
    """Shared code stays board-independent; board code sits behind board.h / audio.h."""

    def test_shared_code_has_no_board_specifics(self):
        board_files = set().union(*BOARD_FILES.values())
        for name, code in SRC.items():
            if os.path.basename(name) in board_files:
                continue
            with self.subTest(file=name):
                self.assertNotRegex(code, r"lv_font_montserrat_\d+|bsp/|GPIO_NUM_\d+|es8311|ES8388",
                                    "pins, fonts and codecs belong in the board files (board_ui())")

    def test_both_boards_implement_the_interfaces(self):
        functions = set()
        for header in ("board.h", "audio.h"):
            functions |= set(re.findall(r"^\w[\w\s\*]*?\b(\w+)\s*\([^;]*\);", repo.read("main", header), re.M))
        self.assertIn("board_display_start", functions)
        for board in ("board_m5stack_tab5.c", "board_tdeck.c"):
            code = SRC[f"main/{board}"]
            for fn in sorted(functions):
                with self.subTest(board=board, function=fn):
                    self.assertRegex(code, rf"\b{fn}\s*\([^;]*\)\s*\{{|\b{fn}\s*\([^;]*\)\s*\{{ return")

    def test_tab5_uses_the_bsp_display(self):
        tab5 = SRC["main/board_m5stack_tab5.c"]
        self.assertIn("bsp_display_start_with_config", tab5, "the BSP detects the Tab5's panel variant")
        self.assertIn("IO_EXPANDER_OUTPUT_MODE_PUSH_PULL", tab5, "expander outputs must be push-pull")
        self.assertNotIn("st7123_lcd_init_cmd_t", tab5, "no fixed single-panel init sequence")

    def test_tab5_wifi_as_simplrssh(self):
        # The C6 link as SimplrSSH's Tab5 firmware: esp_hosted started by the app once the C6 has
        # power, never from a constructor before app_main
        tab5 = dict(re.findall(r"(?m)^(CONFIG_\w+)=(.*)$", repo.read("sdkconfig.defaults.tab5")))
        self.assertEqual(tab5.get("CONFIG_ESP_HOSTED_HOST_RESET_GPIO"), "15")
        self.assertEqual(tab5.get("CONFIG_ESP32P4_TAB5_C6_BOARD"), "y")
        self.assertEqual(tab5.get("CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN"), "n")
        self.assertEqual(repo.sdkconfig("sdkconfig.tab5").get("CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN"), "n")
        self.assertIn("esp_hosted_init()", SRC["main/board_m5stack_tab5.c"])
        manifest = repo.read("main", "idf_component.tab5.yml")
        self.assertIn("espressif/esp_hosted: '==3.0.9'", manifest)
        self.assertIn("espressif/esp_wifi_remote: '==1.6.5'", manifest)

    def test_shared_code_reaches_wifi_through_the_board(self):
        self.assertEqual(grep(r"esp_hosted", exclude=("board_m5stack_tab5.c",)), [],
                         "the C6 link belongs in the Tab5 board file (board_wifi_prepare)")
        self.assertIn("board_wifi_prepare(", SRC["main/wifi_mgr.cpp"])

    def test_tdeck_as_simplrssh(self):
        tdeck = SRC["main/board_tdeck.c"]
        self.assertIn("bsp_display_start_with_config", tdeck)
        self.assertIn("esp_lcd_touch_new_i2c_gt911", tdeck)
        self.assertIn("GPIO_NUM_10", tdeck, "the peripheral power pin")
        defaults = dict(re.findall(r"(?m)^(CONFIG_\w+)=(.*)$", repo.read("sdkconfig.defaults.tdeck")))
        self.assertEqual(defaults.get("CONFIG_IDF_TARGET"), '"esp32s3"')
        self.assertEqual(defaults.get("CONFIG_BSP_DISPLAY_DRIVER_ST7789"), "y")
        self.assertEqual(defaults.get("CONFIG_BSP_I2C_GPIO_SDA"), "18")
        self.assertEqual(defaults.get("CONFIG_BSP_I2C_GPIO_SCL"), "8")


class Ui(unittest.TestCase):
    """UI model (AI_RULES.md): pages, callbacks that only post, LVGL in PSRAM, ASCII."""

    def test_callbacks_only_post_events(self):
        app = SRC["main/app.cpp"]
        for callback in ("on_tap", "on_back", "on_submit", "on_button", "on_swipe"):
            with self.subTest(callback=callback):
                body = app.split(f"s_ui.{callback} = [", 1)[1].split(";", 1)[0]
                self.assertIn("post(Ev::", body, "LVGL callbacks only post an event; the app task does the work")

    def test_blocking_work_in_the_worker(self):
        # Every blocking call sits inside a run_job() in the same function (the worker runs it)
        app = SRC["main/app.cpp"]
        for call in ("wifi::connect(", "wifi::scan(", "wifi::auto_connect(", "wifi::start(", "radio_browser_fetch(",
                     "portal::probe(", "portal::fetch(", "bt_hid::start(", "bt_hid::stop(", "bt_hid::scan(",
                     "bt_hid::pair(", "music::mount(", "music::scan(", "music::load_index(", "music::save_index(",
                     "music::load_genres(", "music::save_genres(", "music::list_folders(", "music_online_artist("):
            for m in re.finditer(re.escape(call), app):
                job = app.rfind("run_job(", 0, m.start())
                with self.subTest(call=call, offset=m.start()):
                    self.assertNotEqual(job, -1, f"{call} outside the worker")
                    self.assertNotIn("\n}\n", app[job:m.start()], f"{call} must run inside a run_job() lambda")

    def test_player_waits_outside_the_display_lock(self):
        app = SRC["main/app.cpp"]
        for call in ("player_stop()", "player_resume()", "player_set_stations(", "player_pause()", "player_play()",
                     "music::play("):
            with self.subTest(call=call):
                for m in re.finditer(re.escape(call), app):
                    before = app[max(0, m.start() - 40):m.start()]
                    self.assertIn("defer(", before, f"{call} waits on tasks: defer it past the display lock")

    def test_lvgl_allocates_in_psram(self):
        self.assertIn("CONFIG_LV_USE_CUSTOM_MALLOC=y", repo.read("sdkconfig.defaults").splitlines())
        mem = repo.strip_comments(repo.read("main", "lv_mem_psram.c"))
        self.assertRegex(mem, r"PSRAM_CAPS\s*=\s*MALLOC_CAP_SPIRAM")
        self.assertIn('"-u lv_malloc_core"', repo.read("main", "CMakeLists.txt"))

    def test_page_model_is_host_tested(self):
        for name in ("main/ui_page.cpp", "main/ui_page.hpp", "main/text.c", "main/tab5_keymap.c",
                     "main/tdeck_keymap.c", "main/hid_report.c", "main/hid_keymap.c", "main/media_tags.c",
                     "main/media_tags.h", "main/music_library.cpp"):
            with self.subTest(file=name):
                self.assertNotRegex(SRC[name], r'#include\s*[<"](lvgl\.h|esp_|freertos/)',
                                    "keep host-tested code free of LVGL / ESP-IDF headers")
        host = repo.read("test", "host", "CMakeLists.txt")
        for name in ("ui_page.cpp", "text.c", "tab5_keymap.c", "tdeck_keymap.c", "portal.cpp", "secret_vault.cpp",
                     "settings.cpp", "hid_report.c", "hid_keymap.c", "bt_store.cpp", "media_tags.c",
                     "music_library.cpp"):
            self.assertIn(f"${{MAIN_DIR}}/{name}", host)

    def test_network_text_cleaned_for_the_screen(self):
        self.assertIn("text_to_ascii(", SRC["main/radio_browser.c"])
        self.assertIn("text_to_ascii(", SRC["main/player.c"])
        self.assertIn("portal::printable(", SRC["main/app.cpp"])
        # Text from the SD card (tags, file names) and from MusicBrainz
        self.assertIn("text_to_ascii(", SRC["main/media_tags.c"])
        self.assertIn("text_to_ascii(", SRC["main/music_online.c"])
        self.assertIn("printable(t.title)", SRC["main/music_library.cpp"])   # The index file's text

    def test_sources_are_ascii(self):
        files = sorted(glob.glob(os.path.join(repo.MAIN, "*.c")) + glob.glob(os.path.join(repo.MAIN, "*.cpp"))
                       + glob.glob(os.path.join(repo.MAIN, "*.h")) + glob.glob(os.path.join(repo.MAIN, "*.hpp")))
        for f in files:
            with open(f, "rb") as fh:
                data = fh.read()
            for i, line in enumerate(data.split(b"\n"), 1):
                if any(b > 0x7F for b in line):
                    with self.subTest(file=os.path.relpath(f, repo.ROOT), line=i):
                        self.fail("non-ASCII byte: use plain ASCII or an escape sequence")


class SdkconfigDefaults(unittest.TestCase):
    FILES = ("sdkconfig.defaults", "sdkconfig.defaults.tab5", "sdkconfig.defaults.tdeck")

    def test_every_setting_explained(self):
        for name in self.FILES:
            lines = repo.read(name).splitlines()
            for i, line in enumerate(lines):
                if not line.startswith("CONFIG_"):
                    continue
                j = i - 1
                while j >= 0 and lines[j].startswith("CONFIG_"):
                    j -= 1
                with self.subTest(file=name, option=line):
                    self.assertTrue(j >= 0 and lines[j].startswith("#"), f"comment each setting in {name}")

    def test_defaults_applied_to_committed_sdkconfig(self):
        for board in ("tab5", "tdeck"):
            committed = repo.sdkconfig(f"sdkconfig.{board}")
            wanted = {}
            for name in ("sdkconfig.defaults", f"sdkconfig.defaults.{board}"):
                wanted.update(re.findall(r"(?m)^(CONFIG_\w+)=(.*)$", repo.read(name)))
            for option, value in wanted.items():
                if option not in committed:
                    continue
                with self.subTest(board=board, option=option):
                    self.assertEqual(committed[option], value, f"sdkconfig.{board} is stale: rebuild and commit it")

    def test_committed_sdkconfig_is_for_the_boards_chip(self):
        for board, chip in (("tab5", '"esp32p4"'), ("tdeck", '"esp32s3"')):
            with self.subTest(board=board):
                self.assertEqual(repo.sdkconfig(f"sdkconfig.{board}").get("CONFIG_IDF_TARGET"), chip)


if __name__ == "__main__":
    unittest.main()
