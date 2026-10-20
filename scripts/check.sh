#!/usr/bin/env bash
# The local check before a push: the guard tests and the host unit tests the
# Tests workflow runs, then SAST with MicroHealth's rules at the newest
# release, the `@main` .github/workflows/security-scan.yml calls (scripts/sast.sh) — the same rules
# CI's security scan runs. It does not build firmware (that needs ESP-IDF).
#
#   scripts/check.sh                # guards, host tests (cmake, g++, libmbedtls-dev), SAST
#   SKIP_SAST=1 scripts/check.sh    # without SAST (no docker/nerdctl or ECR login);
#                                   # say so in the PR — CI's security scan still runs it
#
# SAST needs git, curl, python3, sha256sum and docker or nerdctl logged in to
# MicroHealth's ECR (295669632222); it fails on an ERROR finding.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
(cd test/guards && python3 -m unittest test_supply_chain test_firmware_rules test_rules_file test_ci_scripts \
  test_partitions test_network_surface)
cmake -S test/host -B build-host -DSTRICT=ON -DSANITIZE=ON
cmake --build build-host -j "$(nproc)"
ctest --test-dir build-host --output-on-failure
if [[ "${SKIP_SAST:-}" == 1 ]]; then
  echo "SAST SKIPPED (SKIP_SAST=1): not run locally; CI's security scan still runs it." >&2
  exit 0
fi
scripts/sast.sh "$@"
