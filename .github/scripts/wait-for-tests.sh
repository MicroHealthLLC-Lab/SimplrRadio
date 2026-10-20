#!/usr/bin/env bash
#
# First stage of a release (build.yml, release job). Tests (tests.yml) runs on its own for
# every push; this waits for THOSE runs - the ones for exactly this commit - and passes when
# one of them succeeded. Tests is never run a second time, and no firmware is published from a
# commit that did not pass it. (Same approach as TueriCXO's and SimplrSSH's wait-for-tests.sh.)
#
# A commit can have more than one Tests run: pushed to main and to integration (integration
# fast-forwarded to main) runs on both. Any successful run counts; the stage fails only when
# every run for the commit finished without success.
#
# Env: GH_TOKEN (actions: read), REPO (owner/name), SHA (the commit being released).
# Re-run the failed Tests jobs, then re-run the release workflow: this reads the runs' latest
# attempts.
set -euo pipefail

: "${GH_TOKEN:?}" "${REPO:?}" "${SHA:?}"
FIND_ATTEMPTS="${FIND_ATTEMPTS:-30}"   # x 10 s: the push run is created within seconds
POLL_SECONDS="${POLL_SECONDS:-30}"

runs_url="repos/${REPO}/actions/workflows/tests.yml/runs?event=push&head_sha=${SHA}&per_page=20"
runs() { gh api "$runs_url" --jq '.workflow_runs[] | "\(.id) \(.status) \(.conclusion // "none")"'; }

listing=""
for _ in $(seq 1 "$FIND_ATTEMPTS"); do
    listing="$(runs)"
    [ -n "$listing" ] && break
    sleep 10
done
if [ -z "$listing" ]; then
    echo "::error::No Tests run was started for ${SHA}. Push the commit to a branch first (Tests runs on every push)."
    exit 1
fi

while :; do
    ok="$(awk '$3 == "success" {print $1; exit}' <<<"$listing")"
    if [ -n "$ok" ]; then
        echo "Tests passed for ${SHA}: https://github.com/${REPO}/actions/runs/${ok}"
        exit 0
    fi
    if ! awk '$2 != "completed" {found=1} END {exit !found}' <<<"$listing"; then
        while read -r id _ conclusion; do
            echo "::error::Tests finished '${conclusion}' for ${SHA}: https://github.com/${REPO}/actions/runs/${id}"
        done <<<"$listing"
        exit 1
    fi
    echo "  Tests still running for ${SHA}..."
    sleep "$POLL_SECONDS"
    listing="$(runs)"
done
