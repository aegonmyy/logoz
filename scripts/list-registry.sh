#!/usr/bin/env bash
# Read and decode the chronicle-registry on-chain PDA account.
#
# Usage: ./scripts/list-registry.sh
#   PDA is derived from the built program binary (seed "registry"); override
#   with PDA=<base58>. Wallet home defaults to .scaffold/wallet (WALLET=...).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WALLET="${WALLET:-$REPO/.scaffold/wallet}"
CLI="$REPO/target/debug/chronicle_registry_cli"

# Derive the registry PDA from the program (independent of any pinned config).
PDA="${PDA:-$("$CLI" init-registry --anchorer Public/11111111111111111111111111111111 --dry-run 2>/dev/null \
    | sed -n 's/.*registry → \([1-9A-HJ-NP-Za-km-z]\{32,\}\).*/\1/p' | head -1)}"
[ -n "$PDA" ] || { echo "could not derive registry PDA (is the guest binary built?)" >&2; exit 1; }

# `lgs wallet account get --raw` prints a `$ …` echo line then PRETTY-PRINTS the
# JSON across multiple lines, so slice from the first `{` to the last `}`.
ACCT_JSON="$(LEE_WALLET_HOME_DIR="$WALLET" \
    lgs wallet -- account get --account-id "Public/$PDA" --raw 2>&1 \
    | sed -n '/^{/,/^}/p')"

# Pass the JSON + PDA via the environment — NOT stdin — because `python3 -`
# would consume its program from the heredoc and leave sys.stdin empty.
PDA="$PDA" ACCT_JSON="$ACCT_JSON" python3 <<'PYEOF'
import json, os

pda  = os.environ["PDA"]
text = os.environ.get("ACCT_JSON", "").strip()
if not text:
    print(f"registry PDA {pda}: uninitialized / empty")
    raise SystemExit(0)

raw  = json.loads(text)
data = bytes.fromhex(raw["data"])
pos  = 0

def read_u32():
    global pos
    v = int.from_bytes(data[pos:pos+4], "little"); pos += 4; return v

def read_i64():
    global pos
    v = int.from_bytes(data[pos:pos+8], "little", signed=True); pos += 8; return v

def read_bytes(n):
    global pos
    v = data[pos:pos+n]; pos += n; return v

n = read_u32()
print(f"registry PDA {pda} — entries: {n}")
for _ in range(n):
    klen = read_u32()
    cid  = read_bytes(klen).decode()
    mh   = read_bytes(32).hex()
    ts   = read_i64()
    by_  = read_bytes(32).hex()
    ver  = data[pos]; pos += 1
    print(f"  ts={ts}  cid={cid}  mhash={mh[:16]}…  anchored_by={by_[:16]}…  v{ver}")
PYEOF
