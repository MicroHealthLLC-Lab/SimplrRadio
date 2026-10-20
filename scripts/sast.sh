#!/usr/bin/env bash
# Local SAST: MicroHealth's rules (MicroHealthLLC/sast-rules) at exactly the
# commit this repository's CI runs, run the way CI runs them. Copied from
# sast-rules/local/sast.sh; it reads the workflow reference, so it never needs
# editing.
#
# The reference is a branch (`@main`: every scan change on sast-rules' main is
# a release, so CI always runs the newest one) or a 40-character commit. A
# branch is resolved to its commit NOW, the way GitHub resolves it for CI.
#
#   scripts/sast.sh [--fail-on error|warning|none] [--json out.json]
#
# Needs git, curl, python3, sha256sum and docker or nerdctl logged in to
# MicroHealth's ECR.
set -euo pipefail
root="$(git rev-parse --show-toplevel)"
repo_url=https://github.com/MicroHealthLLC/sast-rules.git
ref="$(grep -rhoE 'MicroHealthLLC/sast-rules/\.github/workflows/[a-z-]+\.yml@[A-Za-z0-9._/-]+' \
  "$root/.github/workflows" | head -1 | sed 's/.*@//')"
[ -n "$ref" ] || { echo "No sast-rules reference in .github/workflows." >&2; exit 2; }
# Run from a git hook, GIT_DIR, GIT_INDEX_FILE and the rest name the
# repository being pushed, and every git command below would act on IT rather
# than on the cache: a pre-push once moved the checkout's HEAD onto the rules
# commit and left an empty cache marked complete. The cache is its own repo.
unset $(git rev-parse --local-env-vars)
if [[ "$ref" =~ ^[0-9a-f]{40}$ ]]; then
  pin="$ref"
else
  # A peeled tag (^{}) names the commit; a branch has only the one line.
  pin="$(git ls-remote "$repo_url" "refs/heads/$ref" "refs/tags/$ref" "refs/tags/$ref^{}" \
    | sort -k2 -r | awk 'NR==1{print $1}')"
  [[ "$pin" =~ ^[0-9a-f]{40}$ ]] || { echo "sast-rules has no branch or tag '${ref}'." >&2; exit 1; }
  echo "sast-rules @${ref} is ${pin}." >&2
fi
cache="${XDG_CACHE_HOME:-$HOME/.cache}/sast-rules/${pin}"
if [ ! -f "$cache/.complete" ] || [ ! -x "$cache/scripts/scan-local.sh" ]; then
  rm -rf "$cache"; mkdir -p "$cache"
  git -C "$cache" init -q
  git -C "$cache" fetch -q --depth 1 "$repo_url" "$pin"
  git -C "$cache" checkout -q FETCH_HEAD
  [ "$(git -C "$cache" rev-parse HEAD)" = "$pin" ] || { echo "sast-rules fetch is not ${pin}" >&2; exit 1; }
  touch "$cache/.complete"
fi
exec "$cache/scripts/scan-local.sh" "$@" "$root"
