#!/usr/bin/env bash
# Read and decode the chronicle-registry on-chain PDA account.
# Usage: ACCOUNT_ID=<base58> ./scripts/list-registry.sh
# Defaults to the account pinned in batch-anchor/batch-anchor.toml.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WALLET="${WALLET:-$REPO/.scaffold/wallet}"
TOML="$REPO/batch-anchor/batch-anchor.toml"

ACCOUNT_ID="${ACCOUNT_ID:-$(grep -E '^signer_account_id' "$TOML" \
    | sed -E 's/.*"([^"]+)".*/\1/')}"

LEE_WALLET_HOME_DIR="$WALLET" \
    lgs wallet -- account get --account-id "Public/$ACCOUNT_ID" --raw 2>&1 \
    | grep -E '^\{' \
    | python3 - <<'PYEOF'
import json, sys

raw = json.loads(sys.stdin.read())
data = bytes.fromhex(raw["data"])
pos = 0

def read_u32():
    global pos
    v = int.from_bytes(data[pos:pos+4], "little")
    pos += 4
    return v

def read_i64():
    global pos
    v = int.from_bytes(data[pos:pos+8], "little", signed=True)
    pos += 8
    return v

def read_bytes(n):
    global pos
    v = data[pos:pos+n]
    pos += n
    return v

n = read_u32()
print(f"entries: {n}")
for _ in range(n):
    klen = read_u32()
    cid  = read_bytes(klen).decode()
    mh   = read_bytes(32).hex()
    ts   = read_i64()
    by_  = read_bytes(32).hex()
    ver  = data[pos]; pos += 1
    print(f"  ts={ts}  cid={cid}  mhash={mh[:16]}…  anchored_by={by_[:16]}…  v{ver}")
PYEOF
