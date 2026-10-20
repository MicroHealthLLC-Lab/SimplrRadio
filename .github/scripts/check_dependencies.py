#!/usr/bin/env python3
"""
Lockfile check for the Tests workflow's install job (from SimplrSSH's). Run after
`idf.py reconfigure` on a clean checkout in the pinned ESP-IDF image:

  check_dependencies.py --board tab5|tdeck <reconfigure.log> <committed-lock> <committed-sdkconfig>
                        <generated-sdkconfig>

(reconfigure rewrites the lock and sdkconfig, so pass copies taken before it ran). Each board
has its lock (dependencies.lock.<board>), its defaults (sdkconfig.defaults, then
sdkconfig.defaults.<board>) and its committed full configuration (sdkconfig.<board>).

Fails (exit 1) when
  - the component manager did not install exactly the versions in the board's lock
    (its first "Processing N dependencies" pass, which also verifies component hashes);
  - a component the build compiles (managed_components/) differs from the lock, unless it
    is listed in KNOWN_DRIFT with the exact version seen today;
  - a setting in the defaults does not take effect in the generated sdkconfig.

ESP-IDF v5.5.1's component manager re-solves dependencies in a second, Kconfig-aware pass on a
clean build directory (some manifests have Kconfig conditions). That pass lists requirements it
does not install (SimplrSSH's Tab5 log shows "lvgl/lvgl (9.6.0~1)" beside the 9.4.0 in
managed_components/, which is what compiles) and rewrites the lock with local paths
(scripts/update-lock.sh keeps the first pass's). This project pins every component with ==,
so the first pass installs exactly the lock; what the second pass lifts is pinned too where the
first pass allows it, and otherwise reviewed in the board's KNOWN_DRIFT. Any other drift in what
managed_components/ holds is a failure instead of a silent firmware change.
"""

import glob
import os
import re
import sys

# component: (locked version, version the build actually compiles today), per board. Each entry
# is reviewed; anything else that drifts fails.
KNOWN_DRIFT = {}
# T-Deck: the first pass needs cmake_utilities 0.x for esp_lcd_ili9341 (an esp_bsp_generic
# dependency); the second pass drops that driver (the T-Deck's panel is an ST7789) and lifts
# cmake_utilities, a CMake helper with no firmware code, to 1.1.1. It can't be pinned in the
# manifest without breaking the first pass.
KNOWN_DRIFT_TDECK = {"espressif/cmake_utilities": ("0.5.3", "1.1.1")}
BOARDS = {
    "tab5": (KNOWN_DRIFT, ["sdkconfig.defaults", "sdkconfig.defaults.tab5"]),
    "tdeck": (KNOWN_DRIFT_TDECK, ["sdkconfig.defaults", "sdkconfig.defaults.tdeck"]),
}


def error(message, file=None):
    where = f" file={file}" if file else ""
    print(f"::error{where}::{message}")


def warning(message, file=None):
    where = f" file={file}" if file else ""
    print(f"::warning{where}::{message}")


def lock_versions(text):
    """{name: version} for the registry components in dependencies.lock."""
    versions = {}
    current = None
    in_deps = False
    for line in text.splitlines():
        if line == "dependencies:":
            in_deps = True
            continue
        if in_deps and re.match(r"^\S", line):
            break
        m = re.match(r"^  ([\w./-]+):$", line)
        if m:
            current = m.group(1)
            continue
        m = re.match(r"^    version:\s*'?([^'\s]+)'?$", line)
        if m and current:
            versions[current] = m.group(1)
    versions.pop("idf", None)
    return versions


def first_install_pass(log):
    """{name: version} from the first 'Processing N dependencies' block of the log."""
    block = re.search(r"Processing (\d+) dependencies:\s*\n((?:.*\n)*?)(?=.*(?:\[\1/\1\]).*\n)", log)
    if not block:
        return None
    count = int(block.group(1))
    lines = re.findall(r"\[(\d+)/%d\] ([\w./-]+) \(([^)]+)\)" % count, log)
    seen = {}
    for index, name, version in lines:
        if name in seen and int(index) == 1:
            break                         # The second pass starts again at [1/N]
        seen[name] = version
    seen.pop("idf", None)
    return seen


def compiled_versions(root):
    """{name: version} of the components in managed_components/ (what the build compiles)."""
    versions = {}
    for manifest in glob.glob(os.path.join(root, "managed_components", "*", "idf_component.yml")):
        folder = os.path.basename(os.path.dirname(manifest))
        name = folder.replace("__", "/", 1)
        with open(manifest, encoding="utf-8") as f:
            m = re.search(r"(?m)^version:\s*'?([^'\s]+)'?", f.read())
        if m:
            versions[name] = m.group(1)
    return versions


def read_sdkconfig(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            m = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line)
            if m:
                values[m.group(1)] = m.group(2)
                continue
            m = re.match(r"^# (CONFIG_[A-Z0-9_]+) is not set$", line)
            if m:
                values[m.group(1)] = "n"
    return values


def check(root, log, committed_lock, committed_cfg, generated_cfg, board):
    known_drift, defaults_files = BOARDS[board]
    problems = []
    with open(committed_lock, encoding="utf-8") as f:
        locked = lock_versions(f.read())

    installed = first_install_pass(log)
    if installed is None:
        problems.append("no 'Processing N dependencies' pass in the reconfigure log")
    elif installed != locked:
        for name in sorted(set(installed) | set(locked)):
            if installed.get(name) != locked.get(name):
                problems.append(f"{name}: installed {installed.get(name)} but the lock has {locked.get(name)} "
                                f"(run BOARD={board} idf.py update-dependencies and commit the lock)")

    compiled = compiled_versions(root)
    drift_rows = []
    for name in sorted(set(compiled) | set(locked)):
        lock_v, built_v = locked.get(name), compiled.get(name)
        if lock_v == built_v:
            continue
        known = known_drift.get(name)
        if known == (lock_v, built_v):
            drift_rows.append((name, lock_v, built_v, "known"))
            warning(f"{name}: the build compiles {built_v}, the lock pins {lock_v} (known re-solve drift)",
                    f"dependencies.lock.{board}")
        else:
            drift_rows.append((name, lock_v, built_v, "NEW"))
            problems.append(f"{name}: the build compiles {built_v} but dependencies.lock.{board} pins {lock_v}")
    for name in sorted(set(known_drift) - {r[0] for r in drift_rows}):
        warning(f"{name} no longer drifts: remove it from KNOWN_DRIFT in check_dependencies.py")

    defaults = {}   # Later files override earlier ones, as in ESP-IDF
    for name in defaults_files:
        with open(os.path.join(root, name), encoding="utf-8") as f:
            defaults.update(re.findall(r"(?m)^(CONFIG_\w+)=(.*)$", f.read()))
    generated = read_sdkconfig(generated_cfg)
    for option, value in defaults.items():
        if generated.get(option, "n") != value and option in generated:
            problems.append(f"{option}={generated[option]} in the generated sdkconfig; the defaults say {value} "
                            f"(rebuild and commit sdkconfig.{board})")

    committed = read_sdkconfig(committed_cfg)
    stale = sorted(k for k in set(committed) | set(generated) if committed.get(k) != generated.get(k))
    if stale:
        warning(f"committed sdkconfig.{board} differs from the generated one in {len(stale)} options "
                "(rebuild and commit it to refresh it)", f"sdkconfig.{board}")
    return problems, drift_rows, len(locked), len(stale)


def main():
    args = sys.argv[1:]
    if len(args) != 6 or args[0] != "--board" or args[1] not in BOARDS:
        print(__doc__)
        return 2
    board = args[1]
    with open(args[2], encoding="utf-8", errors="replace") as f:
        log = f.read()
    problems, drift, count, stale = check(os.getcwd(), log, args[3], args[4], args[5], board)
    for p in problems:
        error(p, f"dependencies.lock.{board}" if "lock" in p else None)

    lines = [f"## Dependencies ({board})", "",
             f"{count} locked components installed with verified hashes: "
             f"{'yes' if not any('installed' in p for p in problems) else '**no**'}.", ""]
    if drift:
        lines += ["| Component | Locked | Compiled | Status |", "|---|---|---|---|"]
        lines += [f"| {n} | {a} | {b} | {s} |" for n, a, b, s in drift]
        lines.append("")
    lines.append(f"Committed sdkconfig.{board} vs generated: {stale} options differ (report only).")
    report = "\n".join(lines) + "\n"
    out = os.environ.get("GITHUB_STEP_SUMMARY")
    if out:
        with open(out, "a") as f:
            f.write(report)
    print(report)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
