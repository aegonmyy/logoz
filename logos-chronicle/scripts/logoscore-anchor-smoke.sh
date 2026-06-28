#!/usr/bin/env bash
# smoke: write a synthetic CID to the on-chain chronicle-registry via Chronicle's
# anchorBatchJson, then verify it appears in the PDA via getRegistryJson.
#
# Prerequisites:
#   1. ffi/target/release/libchronicle_registry_ffi.so must exist
#      (build with: cargo build --release --manifest-path ffi/Cargo.toml)
#   2. A running LEZ localnet (lgs localnet start)
#   3. The chronicle-registry program deployed; ANCHOR_PROGRAM_ID set
#   4. A funded signer: ANCHOR_SIGNER set to base-58 account_id

set -euo pipefail

MODULE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LOGOSCORE="${LOGOSCORE:-/nix/store/bsr21sfvfasl686mfi08r53g68pvc4gk-logos-logoscore-cli-bin-0.1.0/bin/logoscore}"
CHRONICLE_MODS="${CHRONICLE_MODULES:-/tmp/chronicle-next-install/modules}"

source "$(dirname "${BASH_SOURCE[0]}")/lib/load-integration-config.sh"

ANCHOR_PROGRAM_ID="${ANCHOR_PROGRAM_ID:-$IT_PROGRAM_ID}"
ANCHOR_SEQ="${ANCHOR_SEQ:-http://127.0.0.1:3040}"
ANCHOR_WALLET="${ANCHOR_WALLET:-$IT_REPO_ROOT/.scaffold/wallet}"
ANCHOR_SIGNER="${ANCHOR_SIGNER:?ANCHOR_SIGNER must be set (base58 account_id)}"
FFI_LIB="${FFI_LIB:-}"

WORK_DIR="${WORK_DIR:-/tmp/wb-anchor-smoke-$(date +%s)}"
LC_DIR="$WORK_DIR/logoscore"
PER_DIR="$WORK_DIR/persist"
DAEMON_LOG="$WORK_DIR/daemon.log"

mkdir -p "$LC_DIR" "$PER_DIR"

_stop_all() {
    "$LOGOSCORE" --config-dir "$LC_DIR" stop >/dev/null 2>&1 || true
    pkill -TERM -f "$WORK_DIR" 2>/dev/null || true
    sleep 0.5
    pkill -KILL -f "$WORK_DIR" 2>/dev/null || true
    local rem
    rem="$(pgrep -af "$WORK_DIR" | grep -v "pgrep -af" || true)"
    [[ -n "$rem" ]] && { echo "WARN: lingering processes tied to $WORK_DIR:" >&2; echo "$rem" >&2; }
}
trap _stop_all EXIT INT TERM

# Preflight checks
[[ -n "$FFI_LIB" && ! -f "$FFI_LIB" ]] && { echo "FFI_LIB set but .so not found: $FFI_LIB" >&2; exit 1; }
[[ -d "$ANCHOR_WALLET" ]] || { echo "Wallet not found: $ANCHOR_WALLET" >&2; exit 1; }
[[ -d "$CHRONICLE_MODS" ]] || {
    echo "Chronicle modules not found: $CHRONICLE_MODS" >&2
    echo "Build first: nix build path:$MODULE_ROOT#install --out-link /tmp/chronicle-next-install" >&2
    exit 1
}
if ! timeout 2 bash -c "</dev/tcp/$(echo "$ANCHOR_SEQ" | sed -E 's|^https?://||;s|/.*||;s|:|/|')" 2>/dev/null; then
    echo "Sequencer not reachable at $ANCHOR_SEQ — run: lgs localnet start" >&2
    exit 1
fi

printf '  program_id  = %s\n  sequencer   = %s\n  wallet      = %s\n  signer      = %s\n  ffi_lib     = %s\n  work_dir    = %s\n' \
    "$ANCHOR_PROGRAM_ID" "$ANCHOR_SEQ" "$ANCHOR_WALLET" "$ANCHOR_SIGNER" "${FFI_LIB:-<auto>}" "$WORK_DIR"

[[ -n "$FFI_LIB" ]] && export CHRONICLE_FFI_PATH="$FFI_LIB"

"$LOGOSCORE" -D \
    --config-dir "$LC_DIR" --persistence-path "$PER_DIR" \
    -m "$CHRONICLE_MODS" -v >"$DAEMON_LOG" 2>&1 &
disown

sleep 1

"$LOGOSCORE" --config-dir "$LC_DIR" load-module chronicle >/dev/null

CFG_FILE="$WORK_DIR/anchor-cfg.json"
cat >"$CFG_FILE" <<JSON
{
  "program_id":        "$ANCHOR_PROGRAM_ID",
  "sequencer_url":     "$ANCHOR_SEQ",
  "wallet_home":       "$ANCHOR_WALLET",
  "signer_account_id": "$ANCHOR_SIGNER"
}
JSON

SET_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle setAnchorConfigJson @"$CFG_FILE")"
python3 -c '
import json, sys
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
assert res.get("ok") is not False, f"setAnchorConfigJson failed: {res}"
print("config saved")
' <<<"$SET_RESP"

# Init registry PDA if not yet initialised
GET_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle getRegistryJson)"
NEED_INIT="$(python3 -c '
import json, sys
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
print("no" if res.get("ok") is True else "yes")
' <<<"$GET_RESP")"

if [[ "$NEED_INIT" == "yes" ]]; then
    echo "Initialising registry PDA..."
    INIT_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle initRegistryJson)"
    python3 -c '
import json, sys
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
assert res.get("ok") is True, f"initRegistryJson failed: {res}"
print("initialized; tx_hash=" + str(res.get("tx_hash","?"))[:16])
' <<<"$INIT_RESP"
else
    echo "Registry already initialised"
fi

TS="$(date +%s)"
TEST_CID="zWb-smoke-${TS}-$(openssl rand -hex 4)"
TEST_HASH="$(openssl rand -hex 32)"

echo "anchoring cid=$TEST_CID"

REQ_FILE="$WORK_DIR/anchor-req.json"
cat >"$REQ_FILE" <<JSON
{
  "cid":           "$TEST_CID",
  "metadata_hash": "$TEST_HASH",
  "timestamp":     $TS
}
JSON

ANCH_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle anchorBatchJson @"$REQ_FILE")"
TX_HASH="$(python3 -c '
import json, sys
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
assert res.get("ok") is True, f"anchorBatchJson failed: {res}"
th = res.get("tx_hash", "")
assert th, f"no tx_hash: {res}"
print(th)
' <<<"$ANCH_RESP")"
echo "tx_hash=$TX_HASH"

# Verify the record is in the local anchor ledger
LIST_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle listAnchorsJson)"
TEST_CID="$TEST_CID" python3 -c '
import json, sys, os
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
cid  = os.environ["TEST_CID"]
assert res.get("ok") is True
anchors = res.get("anchors", {})
assert cid in anchors, f"{cid} not in anchors map"
rec = anchors[cid]
assert rec["state"] == "confirmed", f"state not confirmed: {rec}"
print("ledger: state=" + rec["state"] + " tx_hash=" + str(rec.get("tx_hash","?"))[:16])
' <<<"$LIST_RESP"

# Poll on-chain registry until the entry appears (up to 60s)
echo "polling on-chain registry..."
OK=""
for i in $(seq 1 30); do
    REG_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle getRegistryJson)"
    if TEST_CID="$TEST_CID" TEST_HASH="$TEST_HASH" TS="$TS" POLL="$i" python3 -c '
import json, sys, os
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
if res.get("ok") is not True: sys.exit(2)
entries = res.get("entries", {})
cid = os.environ["TEST_CID"]
if cid not in entries: sys.exit(1)
rec     = entries[cid]
wh, wt  = os.environ["TEST_HASH"].lower(), int(os.environ["TS"])
gh      = rec.get("metadata_hash","").lower()
gt      = int(rec.get("anchor_timestamp", 0))
assert gh == wh, f"metadata_hash mismatch: chain={gh} sent={wh}"
assert gt == wt, f"anchor_timestamp mismatch: chain={gt} sent={wt}"
print(f"on-chain ok (poll {os.environ[\"POLL\"]})")
' 2>/dev/null; then
        OK="yes"; break
    fi
    sleep 2
done
[[ -n "$OK" ]] || { echo "FAIL: $TEST_CID never appeared on-chain within 60s" >&2; exit 1; }

echo "ok cid=$TEST_CID tx_hash=$TX_HASH work_dir=$WORK_DIR"
