#!/usr/bin/env bash
# End-to-end smoke test for chronicle's on-chain anchor pipeline.
#
# Prerequisites (paths relative to the repo root, which holds the registry
# program/ffi alongside logos-chronicle/ after the LP-17 subtree merge):
#   1. ffi/target/release/libchronicle_registry_ffi.so
#      (from the repo root: cd ffi && cargo build --release)
#   2. Registry program deployed on a running localnet; ANCHOR_PROGRAM_ID
#      must point at the on-chain program id.
#   3. A valid wallet at $ANCHOR_WALLET_HOME with the signer account known.
#
# Flow: load chronicle → setAnchorConfigJson → anchorBatchJson with one
# synthetic CID → assert tx_hash + listAnchorsJson contains a confirmed record.
#
# Each smoke run uses its own RUN_DIR / PERSIST_DIR so it doesn't see anchor
# records from a previous run.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LOGOSCORE="${LOGOSCORE:-/nix/store/bsr21sfvfasl686mfi08r53g68pvc4gk-logos-logoscore-cli-bin-0.1.0/bin/logoscore}"
CHRONICLE_MODULES="${CHRONICLE_MODULES:-/tmp/chronicle-next-install/modules}"

# program_id comes from integration-test.toml; everything else is shared
# with production (sequencer, wallet, signer). Each can be overridden via
# its ANCHOR_* env var.
source "$(dirname "${BASH_SOURCE[0]}")/lib/load-integration-config.sh"
ANCHOR_PROGRAM_ID="${ANCHOR_PROGRAM_ID:-$IT_PROGRAM_ID}"
ANCHOR_SEQUENCER_URL="${ANCHOR_SEQUENCER_URL:-http://127.0.0.1:3040}"
ANCHOR_WALLET_HOME="${ANCHOR_WALLET_HOME:-$IT_REPO_ROOT/.scaffold/wallet}"
ANCHOR_SIGNER_ACCOUNT="${ANCHOR_SIGNER_ACCOUNT:?ANCHOR_SIGNER_ACCOUNT must be set (no static default — base58 account_id from your wallet)}"
FFI_LIB="${FFI_LIB:-}"

RUN_DIR="${RUN_DIR:-/tmp/chronicle-anchor-smoke-$(date +%s)}"
LOG_DIR="$RUN_DIR/logoscore"
PERSIST_DIR="$RUN_DIR/persist"
DAEMON_LOG="$RUN_DIR/daemon.log"

mkdir -p "$LOG_DIR" "$PERSIST_DIR"

cleanup_run() {
  "$LOGOSCORE" --config-dir "$LOG_DIR" stop >/dev/null 2>&1 || true
  pkill -TERM -f "$RUN_DIR" 2>/dev/null || true
  sleep 0.5
  pkill -KILL -f "$RUN_DIR" 2>/dev/null || true
  local survivors
  survivors="$(pgrep -af "$RUN_DIR" | grep -v "pgrep -af" || true)"
  if [[ -n "$survivors" ]]; then
    echo "WARN: processes still tied to $RUN_DIR after cleanup:" >&2
    echo "$survivors" >&2
  fi
}
trap cleanup_run EXIT INT TERM

# ── Preflight ───────────────────────────────────────────────────────────────
# FFI resolution at runtime: env var override > plugin-dir sibling > LD search.
# We only set CHRONICLE_REGISTRY_FFI_PATH below when $FFI_LIB is non-empty —
# the default install bundles the .so next to chronicle_plugin.so, so the
# plugin-dir fallback finds it without configuration.
[[ -n "$FFI_LIB" && ! -f "$FFI_LIB" ]] && { echo "FFI_LIB set but .so not found at $FFI_LIB" >&2; exit 1; }
[[ ! -d "$ANCHOR_WALLET_HOME" ]] && { echo "Wallet home not found at $ANCHOR_WALLET_HOME" >&2; exit 1; }
[[ ! -d "$CHRONICLE_MODULES" ]] && { echo "Chronicle modules not found at $CHRONICLE_MODULES" >&2; echo "Run: nix build path:$ROOT#install --out-link /tmp/chronicle-next-install" >&2; exit 1; }

# Sequencer reachability — bare TCP probe, the JSON-RPC endpoint doesn't
# answer GET / so curl-based checks return 405/etc.
if ! timeout 2 bash -c "</dev/tcp/$(echo "$ANCHOR_SEQUENCER_URL" | sed -E 's|^https?://||;s|/.*||;s|:|/|')" 2>/dev/null; then
  echo "Sequencer not reachable at $ANCHOR_SEQUENCER_URL" >&2
  echo "Run: (from repo root) lgs localnet start" >&2
  exit 1
fi

echo "smoke run: $RUN_DIR"
echo "  program_id   = $ANCHOR_PROGRAM_ID"
echo "  sequencer    = $ANCHOR_SEQUENCER_URL"
echo "  wallet_home  = $ANCHOR_WALLET_HOME"
echo "  signer       = $ANCHOR_SIGNER_ACCOUNT"
echo "  ffi_lib      = $FFI_LIB"

# ── Start logoscore (FFI env var only if FFI_LIB explicitly set) ─────────────
if [[ -n "$FFI_LIB" ]]; then
  export CHRONICLE_REGISTRY_FFI_PATH="$FFI_LIB"
fi
"$LOGOSCORE" -D \
  --config-dir "$LOG_DIR" \
  --persistence-path "$PERSIST_DIR" \
  -m "$CHRONICLE_MODULES" \
  -v >"$DAEMON_LOG" 2>&1 &
disown

sleep 1

"$LOGOSCORE" --config-dir "$LOG_DIR" load-module chronicle >/dev/null

# ── Configure ───────────────────────────────────────────────────────────────
# (config must be set before init/get can run — they read it from disk.)
CONFIG_FILE="$RUN_DIR/anchor-config.json"
cat >"$CONFIG_FILE" <<EOF
{
  "program_id":        "$ANCHOR_PROGRAM_ID",
  "sequencer_url":     "$ANCHOR_SEQUENCER_URL",
  "wallet_home":       "$ANCHOR_WALLET_HOME",
  "signer_account_id": "$ANCHOR_SIGNER_ACCOUNT"
}
EOF
SET_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle setAnchorConfigJson @"$CONFIG_FILE")"
RESULT="$SET_RESULT" python3 -c '
import json, os
w = json.loads(os.environ["RESULT"])
r = json.loads(w["result"])
assert r.get("ok") == True, f"setAnchorConfigJson failed: {r}"
assert r.get("configured") == True, f"not configured after set: {r}"
print("config saved")
'

# ── Ensure registry PDA is initialised ──────────────────────────────────────
# getRegistry succeeds (with empty entries) once the PDA exists. If the
# sequencer doesn't know the PDA, we run initRegistry once. Idempotency note:
# the guest's #[account(init, ...)] errors if the PDA already exists, so we
# only call init when getRegistry has actually failed.
GET_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle getRegistryJson)"
INIT_NEEDED="$(RESULT="$GET_RESULT" python3 -c '
import json, os
w = json.loads(os.environ["RESULT"])
r = json.loads(w["result"])
# get_registry returns ok=true even for empty/fresh PDAs. Failure means the
# sequencer rejected get_account → PDA doesnt exist yet.
print("no" if r.get("ok") is True else "yes")
')"

if [[ "$INIT_NEEDED" == "yes" ]]; then
  echo "registry PDA not reachable yet — calling initRegistryJson"
  INIT_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle initRegistryJson)"
  RESULT="$INIT_RESULT" python3 -c '
import json, os
w = json.loads(os.environ["RESULT"])
r = json.loads(w["result"])
assert r.get("ok") is True, f"init_registry failed: {r}"
print("initialized: tx_hash={}".format(r["tx_hash"][:16]))
'
else
  echo "registry already initialised"
fi

# ── Anchor a synthetic CID ──────────────────────────────────────────────────
STAMP="$(date +%s)"
TEST_CID="zSmoke-${STAMP}-$(openssl rand -hex 4)"
TEST_HASH="$(openssl rand -hex 32)"
REQUEST_FILE="$RUN_DIR/anchor-request.json"
cat >"$REQUEST_FILE" <<EOF
{
  "entries": [
    { "cid": "$TEST_CID", "metadata_hash": "$TEST_HASH", "timestamp": $STAMP }
  ]
}
EOF

echo "anchoring cid=$TEST_CID timestamp=$STAMP"

ANCHOR_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle anchorBatchJson @"$REQUEST_FILE")"
TX_HASH="$(RESULT="$ANCHOR_RESULT" python3 -c '
import json, os
w = json.loads(os.environ["RESULT"])
r = json.loads(w["result"])
assert r.get("ok") == True, f"anchorBatchJson failed: {r}"
tx = r.get("tx_hash")
assert tx, f"no tx_hash in response: {r}"
print(tx)
')"
echo "tx_hash=$TX_HASH"

# ── Verify local persistence ────────────────────────────────────────────────
LIST_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle listAnchorsJson)"
RESULT="$LIST_RESULT" CID="$TEST_CID" python3 -c '
import json, os
w = json.loads(os.environ["RESULT"])
r = json.loads(w["result"])
assert r.get("ok") == True
anchors = r.get("anchors", {})
cid = os.environ["CID"]
assert cid in anchors, f"cid {cid} not in anchors map; got keys: {list(anchors.keys())}"
rec = anchors[cid]
assert rec["state"] == "confirmed", f"state not confirmed: {rec}"
assert rec["tx_hash"], f"missing tx_hash in persisted record: {rec}"
print("persisted: state={} tx_hash={}...".format(rec["state"], rec["tx_hash"][:16]))
'

# ── Verify the on-chain registry actually contains the entry ────────────────
# Strongest proof: read the PDA back, borsh-decode, look for our CID +
# metadata_hash. Sequencer accepts → batches → applies → account updates,
# so the just-anchored entry can take a few seconds to surface; poll.
echo "polling on-chain registry for the new entry..."
ON_CHAIN_OK=""
for i in $(seq 1 30); do
  REG_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle getRegistryJson)"
  if RESULT="$REG_RESULT" CID="$TEST_CID" HASH="$TEST_HASH" STAMP="$STAMP" python3 -c '
import json, os, sys
w = json.loads(os.environ["RESULT"])
r = json.loads(w["result"])
if r.get("ok") is not True:
    sys.exit(2)
entries = r.get("entries", {})
cid = os.environ["CID"]
if cid not in entries:
    sys.exit(1)
rec = entries[cid]
want_hash = os.environ["HASH"].lower()
want_ts   = int(os.environ["STAMP"])
got_hash  = rec.get("metadata_hash", "").lower()
got_ts    = int(rec.get("anchor_timestamp", 0))
assert got_hash == want_hash, f"metadata_hash mismatch: chain={got_hash} sent={want_hash}"
assert got_ts == want_ts, f"anchor_timestamp mismatch: chain={got_ts} sent={want_ts}"
print("on-chain: cid present (poll {}), metadata_hash + anchor_timestamp match".format(os.environ.get("POLL", "?")))
' POLL="$i" 2>/dev/null; then
    ON_CHAIN_OK="yes"
    break
  fi
  sleep 2
done
if [[ -z "$ON_CHAIN_OK" ]]; then
  echo "FAIL: $TEST_CID never appeared in on-chain registry after 60s" >&2
  echo "Last response: $REG_RESULT" >&2
  exit 1
fi

# ── Verify the local cache is used by lookupAnchorJson ──────────────────────
LOOKUP_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle lookupAnchorJson "$TEST_CID")"
RESULT="$LOOKUP_RESULT" python3 -c '
import json, os
w = json.loads(os.environ["RESULT"])
r = json.loads(w["result"])
assert r.get("ok") == True
assert r.get("found") == True, f"lookup missed cached entry: {r}"
print(f"lookup: found")
'

echo "ok cid=$TEST_CID tx_hash=$TX_HASH run_dir=$RUN_DIR"
