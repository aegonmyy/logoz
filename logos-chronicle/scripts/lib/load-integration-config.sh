#!/usr/bin/env bash
# Reads integration-test.toml from the repo root and exports:
#   IT_TOPIC      — waku content topic for smoke-test envelopes
#   IT_PROGRAM_ID — chronicle-registry program id (may be empty)
#
# Exported values are never overwritten when already set in the
# caller's environment — the caller's override takes priority.
# Source this file; do not execute it directly.

set -u

_wb_find_root() {
    local d
    d="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    while [[ "$d" != "/" ]]; do
        [[ -f "$d/integration-test.toml" ]] && { echo "$d"; return 0; }
        d="$(dirname "$d")"
    done
    echo "load-integration-config.sh: integration-test.toml not found" >&2
    return 1
}

IT_REPO_ROOT="${IT_REPO_ROOT:-$(_wb_find_root)}"
export IT_REPO_ROOT

_wb_toml="${IT_REPO_ROOT}/integration-test.toml"
if [[ ! -f "$_wb_toml" ]]; then
    echo "load-integration-config.sh: not found: $_wb_toml" >&2
    return 1 2>/dev/null || exit 1
fi

_wb_eval="$(TOML="$_wb_toml" python3 - <<'PYEOF'
import os, shlex, tomllib
with open(os.environ["TOML"], "rb") as fh:
    cfg = tomllib.load(fh)
def q(v): return shlex.quote(str(v))
print("_WB_TOPIC=" + q(cfg.get("topic", {}).get("content", "")))
print("_WB_PROG="  + q(cfg.get("registry", {}).get("program_id", "")))
PYEOF
)"
eval "$_wb_eval"

IT_TOPIC="${IT_TOPIC:-$_WB_TOPIC}"
IT_PROGRAM_ID="${IT_PROGRAM_ID:-$_WB_PROG}"
export IT_TOPIC IT_PROGRAM_ID

unset _wb_toml _wb_eval _WB_TOPIC _WB_PROG _wb_find_root
