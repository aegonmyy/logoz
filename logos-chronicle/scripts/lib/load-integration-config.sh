#!/usr/bin/env bash
# Load values from the repo-root integration-test.toml. Sourced by smoke
# scripts; not meant to run standalone.
#
# The IT contract is minimal: tests share production's sequencer, wallet,
# signer, and nwaku — only the broadcast content topic differs, so test
# envelopes never reach production consumers.
#
# Exports (env vars win as overrides — pre-set values are kept):
#   IT_TOPIC       — [topic].content
#   IT_PROGRAM_ID  — [registry].program_id

set -u

_it_repo_root() {
    local d
    d="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    while [[ "$d" != "/" ]]; do
        [[ -f "$d/integration-test.toml" ]] && { echo "$d"; return 0; }
        d="$(dirname "$d")"
    done
    return 1
}

IT_REPO_ROOT="${IT_REPO_ROOT:-$(_it_repo_root)}"
IT_CONFIG_PATH="${IT_CONFIG_PATH:-$IT_REPO_ROOT/integration-test.toml}"

if [[ ! -f "$IT_CONFIG_PATH" ]]; then
    echo "load-integration-config.sh: $IT_CONFIG_PATH not found" >&2
    return 1 2>/dev/null || exit 1
fi

_it_vars="$(MAIN="$IT_CONFIG_PATH" python3 - <<'PYEOF'
import os, shlex, tomllib
with open(os.environ["MAIN"], "rb") as f:
    c = tomllib.load(f)
def q(v): return shlex.quote(str(v))
print(f"_IT_TOPIC={q(c['topic']['content'])}")
print(f"_IT_PROGRAM_ID={q(c['registry']['program_id'])}")
PYEOF
)"
eval "$_it_vars"

export IT_TOPIC="${IT_TOPIC:-$_IT_TOPIC}"
export IT_PROGRAM_ID="${IT_PROGRAM_ID:-$_IT_PROGRAM_ID}"

unset _IT_TOPIC _IT_PROGRAM_ID _it_vars _it_repo_root
