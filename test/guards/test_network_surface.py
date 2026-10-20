"""
Network surface (AI_RULES.md, Scope and security): station-only WiFi, IPv4, no listening
sockets, no servers, no OTA of the radio, no Bluetooth. Outbound only, TLS verified with the
CA bundle; plain HTTP only for the captive-portal probe, sign-in pages and http:// streams.
The built firmware is checked too, by check_firmware.py in CI. (After SimplrSSH's test.)
"""

import re
import unittest

import repo

SRC = {name: repo.strip_comments(text) for name, text in repo.sources().items()}
RAW = repo.sources()   # For string literals: strip_comments() also cuts "http://..." at its "//"
ALLOWED_HOSTS = {"connectivitycheck.gstatic.com", "api.radio-browser.info", "de1.api.radio-browser.info",
                 "de2.api.radio-browser.info", "fi1.api.radio-browser.info", "at1.api.radio-browser.info",
                 "example.com", "github.com", "espressif.github.io"}
PROBE = '"http://connectivitycheck.gstatic.com/generate_204"'


def find(pattern):
    return [f"{name}: {m}" for name, code in SRC.items() for m in re.findall(pattern, code)]


def function_body(code, name):
    """Body of the function `name` in `code` (comments already stripped)."""
    m = re.search(r"\b" + name + r"\s*\([^)]*\)\s*(?:const\s*)?\{", code)
    if not m:
        return None
    depth, i = 1, m.end()
    while depth and i < len(code):
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        i += 1
    return code[m.end():i]


class SourceSurface(unittest.TestCase):
    def test_no_listening_sockets(self):
        self.assertEqual(find(r"\b(?:lwip_)?(?:listen|accept|bind)\s*\(\s*\w"), [])

    def test_station_mode_only(self):
        self.assertEqual(find(r"WIFI_MODE_(?:AP|APSTA)\b"), [])
        self.assertEqual(find(r"esp_netif_create_default_wifi_ap"), [])
        self.assertTrue(find(r"WIFI_MODE_STA\b"))

    def test_ipv4_only(self):
        self.assertEqual(find(r"\bAF_INET6\b|\bAF_UNSPEC\b"), [])

    def test_no_servers_ota_bluetooth_mdns(self):
        forbidden = r'#include\s*[<"](?:esp_http_server|esp_https_server|esp_https_ota|esp_ota_ops|' \
                    r'esp_bt|esp_gap|esp_gatt|nimble|mdns|esp_now|esp_wifi_ap|esp_local_ctrl|captive_portal|' \
                    r'wifi_prov)[^>"]*[>"]'
        hits = [h for h in find(forbidden) if "esp_ota_ops" not in h or "settings_nvs.cpp" not in h]
        self.assertEqual(hits, [], "esp_ota_ops is allowed only in settings_nvs.cpp (running partition)")

    def test_only_known_endpoints(self):
        for name, text in repo.sources().items():
            for host in re.findall(r"https?://([A-Za-z0-9.-]+)", text):
                with self.subTest(file=name, host=host):
                    self.assertIn(host, ALLOWED_HOSTS)

    def test_plain_http_only_for_the_portal_probe(self):
        hits = [(name, m) for name, text in repo.sources().items() for m in re.findall(r'"http://[^"]+"', text)]
        self.assertEqual(hits, [("main/portal_client.cpp", PROBE)],
                         "plain HTTP addresses: only the captive-portal probe")

    def test_directory_over_https(self):
        self.assertIn('"https://%s%s&limit=%d"', RAW["main/radio_browser.c"])

    def test_streams_only_web_urls(self):
        self.assertIn('strncasecmp(url, "http://", 7)', RAW["main/stream.c"])
        self.assertIn("web_url(url->valuestring)", SRC["main/radio_browser.c"])

    def test_tls_verified_with_ca_bundle(self):
        for client in ("main/portal_client.cpp", "main/radio_browser.c", "main/stream.c"):
            with self.subTest(file=client):
                self.assertRegex(SRC[client], r"crt_bundle_attach\s*=\s*esp_crt_bundle_attach")
        for name, code in SRC.items():
            with self.subTest(file=name):
                self.assertNotRegex(code, r"skip_cert_common_name_check\s*=\s*true|use_global_ca_store|"
                                          r"MBEDTLS_SSL_VERIFY_NONE|MBEDTLS_SSL_VERIFY_OPTIONAL")


class SignInPages(unittest.TestCase):
    """Captive portal: checked after a connection the user made, opened when asked, nothing kept."""

    PORTAL_FILES = ("main/portal.cpp", "main/portal_client.cpp")

    def test_background_reconnects_load_no_page(self):
        app = SRC["main/app.cpp"]
        after = function_body(app, "after_connect")
        self.assertIsNotNone(after)
        self.assertRegex(after, r"if \(!quiet\) \{\s*portal_wipe\(\);\s*portal_load\(",
                         "only a connection the user made checks")
        house = function_body(app, "housekeeping")
        self.assertNotIn("portal", house)
        self.assertIn("auto_connect(true)", house)

    def test_pages_load_only_in_the_worker(self):
        fetchers = sorted({name for name, code in SRC.items() if re.search(r"portal::(?:fetch|probe)\s*\(", code)})
        self.assertEqual(fetchers, ["main/app.cpp"])

    def test_nothing_from_a_page_is_saved(self):
        for name in self.PORTAL_FILES:
            with self.subTest(file=name):
                self.assertNotRegex(SRC[name], r"settings_nvs|nvs_set|fopen|vault::put")
        portal_part = RAW["main/app.cpp"].split("// Sign-in page (captive portal)", 1)[1].split("\n// Boot\n", 1)[0]
        self.assertNotRegex(portal_part, r"settings_|nvs_set|vault::put|wifi::remember")

    def test_page_state_wiped_when_the_sign_in_ends(self):
        wipe = function_body(SRC["main/app.cpp"], "portal_wipe")
        self.assertIn("portal::wipe(s_portal)", wipe)
        self.assertIn("s_cookies.wipe()", wipe)
        self.assertIn("mbedtls_platform_zeroize", SRC["main/portal_client.cpp"])

    def test_portal_parser_is_host_tested(self):
        parser = SRC["main/portal.cpp"] + SRC["main/portal.hpp"]
        self.assertNotRegex(parser, r'#include\s*"(?:esp_|freertos|lvgl)')
        self.assertIn("portal.cpp", repo.read("test", "host", "CMakeLists.txt"))


# The same options check_firmware.py enforces on the sdkconfig the build generates
check_firmware = repo.ci_script("check_firmware")


class ConfigSurface(unittest.TestCase):
    def test_committed_sdkconfigs(self):
        for board in ("tab5", "tdeck"):
            with self.subTest(board=board):
                self.assertEqual(check_firmware.sdkconfig_problems(repo.sdkconfig(f"sdkconfig.{board}")), [])

    def test_defaults_declare_the_surface(self):
        defaults = repo.read("sdkconfig.defaults")
        for option in ("CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n", "CONFIG_LWIP_DHCPS=n", "CONFIG_LWIP_IPV6=n",
                       "CONFIG_BT_ENABLED=n", "CONFIG_ESP_WIFI_NVS_ENABLED=n", "CONFIG_ESP_TLS_INSECURE=n"):
            with self.subTest(option=option):
                self.assertIn(option, defaults)
        # The Tab5's WiFi driver runs on its C6: the same surface on the co-processor's side
        tab5 = repo.read("sdkconfig.defaults.tab5")
        for option in ("CONFIG_WIFI_RMT_SOFTAP_SUPPORT=n", "CONFIG_WIFI_RMT_NVS_ENABLED=n",
                       "CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=n", "CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID=n"):
            with self.subTest(option=option):
                self.assertIn(option, tab5)

    def test_wifi_password_not_stored_by_the_driver(self):
        self.assertIn("esp_wifi_set_storage(WIFI_STORAGE_RAM)", SRC["main/wifi_mgr.cpp"])


if __name__ == "__main__":
    unittest.main()
