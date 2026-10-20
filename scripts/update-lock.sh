#!/usr/bin/env bash
# Regenerates a board's component lock (dependencies.lock.<board>) from a clean tree, in the
# pinned ESP-IDF image, and keeps it the way the component manager writes it in its first
# pass: registry components with their hashes.
#
#   scripts/update-lock.sh tab5|tdeck
#
# Why not plain `idf.py update-dependencies`: LVGL 9.6's manifest has Kconfig-conditional
# dependencies, so a configure from an empty build directory runs the component manager a
# second time once Kconfig is known, and that pass rewrites the lock with local paths
# (managed_components/...) instead of hashes. Builds compile the same versions either way
# (every component is pinned with ==), but only the first-pass lock may be committed
# (test/guards/test_supply_chain.py checks it). Also refreshes sdkconfig.<board>.
set -euo pipefail
board="${1:?usage: scripts/update-lock.sh tab5|tdeck}"
case "$board" in tab5|tdeck) ;; *) echo "unknown board: $board" >&2; exit 2 ;; esac
cd "$(git rev-parse --show-toplevel)"
image="espressif/idf:v5.5.1@sha256:dfa2d076c796769c07c155eba6c672b9f395aec943b2ba3701b73379b5f9e884"
lock="dependencies.lock.$board"
captured="$(mktemp)"
trap 'rm -f "$captured"' EXIT
rm -rf managed_components "build-$board" "$lock"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e "BOARD=$board" -v "$PWD:/project" -w /project \
  "$image" idf.py -B "build-$board" reconfigure > "reconfigure-$board.log" 2>&1 &
pid=$!
while kill -0 "$pid" 2>/dev/null; do
  if [ ! -s "$captured" ] && [ -f "$lock" ] && grep -q "type: service" "$lock" && ! grep -q "type: local" "$lock"; then
    cp "$lock" "$captured"
  fi
  sleep 0.05
done
wait "$pid" || { tail -40 "reconfigure-$board.log"; exit 1; }
[ -s "$captured" ] || { echo "no first-pass lock seen; see reconfigure-$board.log" >&2; exit 1; }
cp "$captured" "$lock"
echo "Wrote $lock ($(grep -c 'type: service' "$lock") registry components) and sdkconfig.$board"
