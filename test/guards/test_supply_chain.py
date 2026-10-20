"""
Supply chain: every GitHub Action is pinned to a commit SHA, every container image to a
digest, and every ESP-IDF component to an exact version recorded in each board's lock file
(dependencies.lock.tab5, dependencies.lock.tdeck). From SimplrSSH's test_supply_chain.py.
"""

import os
import re
import unittest

import repo

USES = re.compile(r"^\s*(?:-\s*)?uses:\s*([^\s#]+)", re.M)
PINNED = re.compile(r"^[\w.-]+/[\w./-]+@[0-9a-f]{40}$")

# The ONE exception to SHA pinning, by the owner's decision: MicroHealth's shared security
# workflows are called @main. sast-rules' main IS its release branch (every scan change pushed
# there is released), and its workflows check out their own rules at the commit the job runs
# (the OIDC job_workflow_sha), so @main runs the newest reviewed release with no pin to move.
# Exactly that repository's workflows at exactly `main`; every other ref stays SHA-pinned.
SAST_RULES_AT_MAIN = re.compile(r"^MicroHealthLLC/sast-rules/\.github/workflows/[a-z0-9-]+\.yml@main$")


def pinned_or_named_exception(ref):
    return bool(PINNED.match(ref) or SAST_RULES_AT_MAIN.match(ref))


class ActionsPinned(unittest.TestCase):
    def test_actions_pinned_to_commit_sha(self):
        for name, text in repo.workflows().items():
            for ref in USES.findall(text):
                if ref.startswith("./"):
                    continue                      # Reusable workflow in this repository
                with self.subTest(workflow=name, uses=ref):
                    self.assertTrue(pinned_or_named_exception(ref),
                                    f"{ref}: pin actions to a full commit SHA (with a # vX.Y.Z comment)")

    def test_only_sast_rules_at_main_is_unpinned(self):
        sha = "206c21cb92451de53799c90ea7dedada6c256e2e"
        for ok in ("MicroHealthLLC/sast-rules/.github/workflows/security-scan.yml@main",
                   f"actions/checkout@{sha}",
                   f"MicroHealthLLC/sast-rules/.github/workflows/security-scan.yml@{sha}"):
            with self.subTest(accepted=ok):
                self.assertTrue(pinned_or_named_exception(ok))
        for bad in ("actions/checkout@main",
                    "MicroHealthLLC/other-repo/.github/workflows/security-scan.yml@main",
                    "MicroHealthLLC/sast-rules/.github/workflows/security-scan.yml@integration",
                    "MicroHealthLLC/sast-rules/.github/workflows/security-scan.yml@v2026.10.15",
                    "MicroHealthLLC/sast-rules/.github/workflows/security-scan.yml@main2",
                    "MicroHealthLLC/sast-rules/action@main",
                    "evil/MicroHealthLLC/sast-rules/.github/workflows/security-scan.yml@main"):
            with self.subTest(refused=bad):
                self.assertFalse(pinned_or_named_exception(bad))

    def test_container_images_pinned_by_digest(self):
        image = re.compile(r"(?:docker (?:pull|run)[^\n]*?\s|esp_idf_version:\s*)"
                           r"((?:[\w.-]+/)*[\w.-]+:[\w.-]+(?:@sha256:[0-9a-f]{64})?)")
        for name, text in repo.workflows().items():
            for ref in image.findall(text):
                with self.subTest(workflow=name, image=ref):
                    self.assertRegex(ref, r"@sha256:[0-9a-f]{64}$", "pin images by digest")
            for ref in re.findall(r"esp_idf_version:\s*([^\s#]+)", text):
                with self.subTest(workflow=name, esp_idf_version=ref):
                    self.assertRegex(ref, r"@sha256:[0-9a-f]{64}$")

    def test_image_variables_and_known_images_pinned(self):
        for name, text in repo.workflows().items():
            code = re.sub(r"(?m)\s#\s.*$", "", text)          # Comments may name an image by tag
            for var, ref in re.findall(r"(?m)^\s*(\w*IMAGE\w*):\s*([\w.-]+/[^\s#]+:[^\s#]+)", code):
                with self.subTest(workflow=name, variable=var):
                    self.assertRegex(ref, r"@sha256:[0-9a-f]{64}$")
            for ref in re.findall(r"(?:espressif/idf|zricethezav/gitleaks|koalaman/shellcheck):[^\s'\"]*", code):
                with self.subTest(workflow=name, image=ref):
                    self.assertRegex(ref, r"@sha256:[0-9a-f]{64}$")

    def test_run_images_pulled_with_retries(self):
        # A registry hiccup (Docker Hub auth timeout) must not fail the gate: `docker run`
        # takes its image from an *_IMAGE variable that a retry loop pulled first.
        for name, text in repo.workflows().items():
            code = re.sub(r"(?m)\s#\s.*$", "", text).replace("\\\n", " ")
            for cmd in re.findall(r"docker run\b[^\n]*", code):
                with self.subTest(workflow=name, run=cmd[:60]):
                    self.assertNotRegex(cmd, r"\s[\w.-]+/[\w.-]+:[\w.-]+@sha256:",
                                        "run images through an *_IMAGE variable")
                    for var in re.findall(r'"\$(\w*IMAGE\w*)"', cmd):
                        self.assertRegex(code, r'for attempt in [^\n]*\n\s*docker pull "\$' + var + r'" && exit 0',
                                         f"pull ${var} with retries before running it")

    def test_workflows_default_to_read_only_token(self):
        for name, text in repo.workflows().items():
            with self.subTest(workflow=name):
                self.assertRegex(text, r"(?m)^\s*permissions:", "declare token permissions")
                self.assertNotRegex(text, r"permissions:\s*write-all")

    def test_no_pull_request_target(self):
        for name, text in repo.workflows().items():
            with self.subTest(workflow=name):
                self.assertNotIn("pull_request_target", text)

    def test_dependabot_keeps_actions_current(self):
        self.assertIn("package-ecosystem: github-actions", repo.read(".github", "dependabot.yml"))


BOARDS = {"tab5": "esp32p4", "tdeck": "esp32s3"}   # Board: chip


def manifest_dependencies(board):
    """{name: version spec} from main/idf_component.<board>.yml (`name: '==x.y.z'`)."""
    deps = {}
    for line in repo.read("main", f"idf_component.{board}.yml").splitlines():
        m = re.match(r"^  ([\w./-]+):\s*'([^']+)'", line)
        if m:
            deps[m.group(1)] = m.group(2)
    return deps


def lock_entries(name):
    """{name: {'version':..., 'component_hash':..., 'type':...}} from a lock file."""
    entries = {}
    current = None
    in_deps = False
    for line in repo.read(name).splitlines():
        if line == "dependencies:":
            in_deps = True
            continue
        if in_deps and re.match(r"^\S", line):
            in_deps = False
        if not in_deps:
            continue
        m = re.match(r"^  ([\w./-]+):$", line)
        if m:
            current = entries.setdefault(m.group(1), {})
            continue
        m = re.match(r"^    (version|component_hash):\s*'?([^'\s]+)'?$", line)
        if m and current is not None:
            current[m.group(1)] = m.group(2)
        m = re.match(r"^      type:\s*(\w+)$", line)
        if m and current is not None:
            current["type"] = m.group(1)
    return entries


class ComponentsPinned(unittest.TestCase):
    def test_direct_dependencies_exact(self):
        for board in BOARDS:
            for name, version in manifest_dependencies(board).items():
                if name == "idf":
                    continue
                with self.subTest(board=board, component=name):
                    self.assertRegex(version, r"^==\d+\.\d+\.\d+(~\d+)?$", "pin components with ==x.y.z")

    def test_direct_dependencies_locked_at_same_version(self):
        for board, chip in BOARDS.items():
            lock_name = f"dependencies.lock.{board}"
            lock = lock_entries(lock_name)
            with self.subTest(lock=lock_name):
                self.assertIn(f"target: {chip}", repo.read(lock_name))
            for name, version in manifest_dependencies(board).items():
                if name == "idf":
                    continue
                with self.subTest(lock=lock_name, component=name):
                    self.assertIn(name, lock, "run idf.py update-dependencies for this board and commit its lock")
                    self.assertEqual(lock[name].get("version"), version.lstrip("="))

    def test_registry_components_have_hashes(self):
        for board in BOARDS:
            lock_name = f"dependencies.lock.{board}"
            for name, info in lock_entries(lock_name).items():
                if name == "idf":
                    continue
                with self.subTest(lock=lock_name, component=name):
                    self.assertEqual(info.get("type"), "service", "only registry components (no local paths)")
                    self.assertRegex(info.get("component_hash", ""), r"^[0-9a-f]{64}$")

    def test_board_locks_selected_by_board(self):
        cmake = repo.read("CMakeLists.txt")
        lock = 'idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${BOARD}")'
        self.assertIn(lock, cmake)
        self.assertIn('configure_file("${CMAKE_SOURCE_DIR}/main/idf_component.${BOARD}.yml"', cmake)

    def test_generated_files_not_committed(self):
        ignored = repo.read(".gitignore")
        for name in ("main/idf_component.yml", "board_config.cmake", "/sdkconfig"):
            with self.subTest(file=name):
                self.assertIn(name, ignored)

    def test_ci_idf_version_matches_manifests(self):
        build = repo.read(".github", "workflows", "build.yml")
        ci = re.search(r"ESP_IDF_VERSION:\s*v([\d.]+)", build).group(1)
        for board in BOARDS:
            idf = manifest_dependencies(board)["idf"]
            with self.subTest(board=board):
                self.assertEqual(idf.lstrip("~^="), ci, f"manifest idf {idf} vs CI v{ci}")


class SecurityScan(unittest.TestCase):
    """One security workflow: MicroHealth's shared security-scan (SAST, Trivy, SBOM, ZAP DAST),
    every scanner on, and the same rules locally through scripts/sast.sh."""

    # @main: the newest sast-rules release (see SAST_RULES_AT_MAIN above).
    SHARED = re.compile(r"uses:\s*MicroHealthLLC/sast-rules/\.github/workflows/security-scan\.yml@main(?:\s|$)")

    def test_one_security_scan_calls_the_shared_workflow(self):
        text = repo.read(".github", "workflows", "security-scan.yml")
        self.assertRegex(text, self.SHARED)
        callers = [n for n, t in repo.workflows().items() if "MicroHealthLLC/sast-rules/" in t]
        self.assertEqual(callers, [".github/workflows/security-scan.yml"],
                         "SAST, Trivy, SBOM and ZAP run from the one security-scan workflow")

    def test_every_scanner_on(self):
        text = repo.read(".github", "workflows", "security-scan.yml")
        for scanner in ("sast", "trivy", "sbom", "zap"):
            with self.subTest(scanner=scanner):
                self.assertNotRegex(text, rf"(?m)^\s*{scanner}:\s*false", "every scanner stays on")
        self.assertIn("zap_target: ${{ vars.ZAP_TARGET_URL }}", text)

    def test_local_sast_uses_the_same_rules(self):
        sast = repo.path("scripts", "sast.sh")
        self.assertTrue(os.access(sast, os.X_OK), "scripts/sast.sh must be executable")
        self.assertIn("MicroHealthLLC/sast-rules", repo.read("scripts", "sast.sh"))
        self.assertIn("scripts/sast.sh", repo.read("scripts", "check.sh"))


if __name__ == "__main__":
    unittest.main()
