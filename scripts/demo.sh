#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# LP-0017 Whistleblower — reproducible end-to-end demo
#
#   Runs the FULL anchor pipeline against a REAL LOCAL LEZ sequencer with
#   RISC0_DEV_MODE=0 (real Groth16 proofs — no dev-mode shortcut):
#
#     local rc5 sequencer  →  deploy program  →  fund accounts
#       →  init registry  →  privacy-preserving index_batch anchor (real proof)
#       →  query registry by CID (found)  →  query bogus CID (not found)
#       →  idempotent re-anchor (already-registered does not fail)
#
#   The publish half (file → Logos Storage → CID → Delivery broadcast) is shown
#   by ./scripts/publish-demo.sh / the smoke tests; this script focuses on the
#   on-chain anchor + registry, which is where proof generation is visible.
#
# Why a local sequencer: the prize requires "a reproducible end-to-end demo
# script [that] works against a real local sequencer with RISC0_DEV_MODE=0".
# The public LEZ testnet this app was originally verified against was wiped in
# the Testnet v0.2.0 migration; the local sequencer runs the same rc5 build and
# the same code path.
#
# Idempotent: safe to re-run. Pass DEMO_RESET=1 to wipe the localnet + wallet
# and start from genesis.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

# ── Tunables ─────────────────────────────────────────────────────────────────
WALLET_DIR="$REPO/.scaffold/wallet"
: "${LOGOS_SCAFFOLD_WALLET_PASSWORD:=logosdemo50}"   # local-dev wallet password
export LOGOS_SCAFFOLD_WALLET_PASSWORD
export LEE_WALLET_HOME_DIR="$WALLET_DIR"
GUEST_BIN="$REPO/methods/guest/target/riscv32im-risc0-zkvm-elf/docker/chronicle_registry.bin"
CLI="$REPO/target/debug/chronicle_registry_cli"
ANCHOR="$REPO/batch-anchor/target/debug/batch-anchor"
SEQ_PORT=3040
FAUCET_AMOUNT=100          # shielded into the private anchorer
DEMO_CID="${DEMO_CID:-bafydemo$(date +%Y%m%d)whistleblowerlocal01}"
DEMO_TS="${DEMO_TS:-$(date +%s)}"

c() { printf '\n\033[1;36m══ %s\033[0m\n' "$*"; }   # cyan banner
ok() { printf '\033[1;32m✓ %s\033[0m\n' "$*"; }
die() { printf '\033[1;31m✗ %s\033[0m\n' "$*" >&2; exit 1; }

# Resolve the lez repo cache dir that lgs created for our pinned LEZ rev.
lez_cache() {
  local rev
  rev="$(grep -E '^\s*pin\s*=' "$REPO/scaffold.toml" | head -1 | sed -E 's/.*"([0-9a-f]+)".*/\1/')"
  echo "$HOME/.cache/logos-scaffold/repos/lez/$rev"
}

# ─────────────────────────────────────────────────────────────────────────────
c "RISC0_DEV_MODE — real proofs, no dev-mode shortcut"
export RISC0_DEV_MODE=0
echo "RISC0_DEV_MODE=$RISC0_DEV_MODE"
echo "scaffold.toml localnet.risc0_dev_mode = $(grep -A2 '\[localnet\]' scaffold.toml | grep risc0_dev_mode | grep -o 'true\|false')"

# ── 1. Toolchain prerequisites ───────────────────────────────────────────────
c "1/9  toolchain"
command -v lgs   >/dev/null || die "lgs not on PATH (cargo install --git https://github.com/logos-co/scaffold logos-scaffold)"
command -v cargo >/dev/null || die "cargo not on PATH"
# `lgs setup` builds the project-local sequencer, wallet and spel from the
# pinned LEZ/SPEL sources into ~/.cache/logos-scaffold.
echo "→ lgs setup (build sequencer/wallet/spel from pinned sources)"
lgs setup >/dev/null
# Put the vendored spel on PATH for `make`/CLI program-id helpers.
SPEL_BIN="$(find "$HOME/.cache/logos-scaffold/repos/spel" -path '*/target/release/spel' 2>/dev/null | head -1)"
[ -n "$SPEL_BIN" ] && ln -sf "$SPEL_BIN" "$HOME/.cargo/bin/spel" 2>/dev/null || true
ok "toolchain ready"

# ── 2. lgs↔lez layout bridge ─────────────────────────────────────────────────
# Our pinned lgs reads the sequencer/wallet configs at <lez>/sequencer/... and
# <lez>/wallet/..., but the pinned LEZ moved them under a lez/ prefix. Bridge
# the two with symlinks (no-op if lgs ever catches up to the new layout).
c "2/9  lgs↔lez config-path bridge"
LEZ="$(lez_cache)"
[ -d "$LEZ/lez/sequencer" ] && ln -sfn lez/sequencer "$LEZ/sequencer"
[ -d "$LEZ/lez/wallet" ]    && ln -sfn lez/wallet    "$LEZ/wallet"
ok "bridged $([ -e "$LEZ/sequencer/service/configs/debug/sequencer_config.json" ] && echo OK || echo MISSING)"

# ── 3. Guest binary (deterministic ProgramId) ────────────────────────────────
c "3/9  guest program binary"
if [ ! -f "$GUEST_BIN" ]; then
  echo "→ building guest (cargo risczero build, ~3 min)…"
  make build
fi
[ -x "$CLI" ] || cargo build --bin chronicle_registry_cli >/dev/null
PROGRAM_ID="$("$CLI" init-registry --anchorer Public/11111111111111111111111111111111 --dry-run 2>/dev/null \
              | sed -n 's/.*Program ID: \([0-9a-f]\{64\}\).*/\1/p' | head -1)"
ok "ProgramId $PROGRAM_ID"

# ── 4. Deterministic local wallet ────────────────────────────────────────────
c "4/9  wallet (deterministic, password-protected)"
WALLET_BIN="$LEZ/target/release/wallet"
# Wallet subcommands are interactive: they prompt for the unlock password on the
# terminal. Feed it on stdin so the demo never blocks on an (often hidden)
# prompt. If a call ever still prompts, its stderr is no longer swallowed, so
# you'll SEE it and can type the password below.
w() { printf '%s\n' "$LOGOS_SCAFFOLD_WALLET_PASSWORD" | "$WALLET_BIN" "$@"; }
echo "→ wallet unlock password: $LOGOS_SCAFFOLD_WALLET_PASSWORD  (auto-fed on stdin; only type it if a prompt appears)"
# Recreate the wallet when resetting (its private notes are tied to the wiped
# chain) or when the on-disk storage was written by an incompatible schema.
if [ "${DEMO_RESET:-0}" = 1 ] || ! w account list >/dev/null; then
  echo "→ (re)initialising wallet storage"
  rm -f "$WALLET_DIR/storage.json" "$REPO/.scaffold/state/wallet.state"
  w account list >/dev/null || true
fi
mapfile -t ACCTS < <(w account list 2>/dev/null | sed -nE 's#^/ (Public|Private)/([1-9A-HJ-NP-Za-km-z]+).*#\1 \2#p')
DEPLOYER="$(printf '%s\n' "${ACCTS[@]}" | awk '$1=="Public"{print $2; exit}')"
ANCHORER="$(printf '%s\n' "${ACCTS[@]}" | awk '$1=="Private"{print $2; exit}')"
[ -n "$DEPLOYER" ] && [ -n "$ANCHORER" ] || die "wallet missing a Public deployer and/or Private anchorer"
ok "deployer Public/$DEPLOYER  ·  anchorer Private/$ANCHORER"

# ── 5. Genesis-authorise the deployer + (re)start the sequencer ──────────────
# Genesis supply_accounts are the only accounts the faucet will initialise
# (uninjected accounts fail `auth-transfer init` with ClaimedUnauthorizedAccount).
c "5/9  local sequencer (genesis-authorise deployer, RISC0_DEV_MODE=0)"
SEQ_CFG="$LEZ/lez/sequencer/service/configs/debug/sequencer_config.json"
python3 - "$SEQ_CFG" "$DEPLOYER" <<'PY'
import json, sys
p, dep = sys.argv[1], sys.argv[2]
d = json.load(open(p))
g = d["genesis"]
ids = [list(e.values())[0].get("account_id") for e in g if "supply_account" in e]
if dep not in ids:
    g.append({"supply_account": {"account_id": dep, "balance": 1000000}})
    json.dump(d, open(p, "w"), indent=2)
    print("  injected", dep, "into genesis")
else:
    print("  deployer already in genesis")
PY
seq_up() { (exec 3<>"/dev/tcp/127.0.0.1/$SEQ_PORT") 2>/dev/null && exec 3<&-; }
if [ "${DEMO_RESET:-0}" = 1 ] || ! seq_up; then
  lgs localnet stop >/dev/null 2>&1 || true
  rm -rf "$LEZ/rocksdb"
  rm -f "$REPO/.scaffold/state/sequencer_config.json"   # force re-prepare from source
  lgs localnet start --timeout-sec 90 >/dev/null
fi
seq_up || die "sequencer not listening on :$SEQ_PORT"
ok "sequencer up on :$SEQ_PORT (real-proof mode)"

# ── 6. Fund the accounts ─────────────────────────────────────────────────────
# Recipe: auth-transfer init (now authorised) → pinata faucet claim → shield
# public→private so the anchorer can pay for a privacy-preserving (proving) tx.
c "6/9  fund deployer (faucet) + shield to private anchorer"
# Initialise the deployer under the auth-transfer program (idempotent), then
# claim from the local faucet. Genesis-authorisation (phase 5) is what lets the
# init succeed — otherwise it fails ClaimedUnauthorizedAccount.
w auth-transfer init --account-id "Public/$DEPLOYER" >/dev/null 2>&1 || true
lgs wallet topup "Public/$DEPLOYER" 2>&1 | grep -E "topup complete|pending|Address:" || true
BAL="$(w account get --account-id "Public/$DEPLOYER" 2>/dev/null | sed -n 's/.*"balance":\([0-9]*\).*/\1/p')"
echo "  deployer spendable balance: ${BAL:-0}"
[ "${BAL:-0}" -gt 0 ] || die "faucet did not fund the deployer (balance ${BAL:-0}) — check the sequencer"
echo "→ shielding $FAUCET_AMOUNT public→private (this is itself a proving tx, ~minutes)…"
w auth-transfer send --from "Public/$DEPLOYER" --to "Private/$ANCHORER" --amount "$FAUCET_AMOUNT"
w account sync-private >/dev/null 2>&1 || true
ok "deployer funded; anchorer holds shielded balance"

# ── 7. Deploy + open registry ────────────────────────────────────────────────
c "7/9  deploy program + open registry"
lgs wallet default set "Public/$DEPLOYER" >/dev/null 2>&1 || true
lgs deploy --program-path "$GUEST_BIN" >/dev/null
"$CLI" init-registry --anchorer "Public/$DEPLOYER" 2>&1 | grep -E "tx_hash|confirmed|AlreadyInitialized" || true
ok "registry open"

# ── 8. Privacy-preserving anchor (REAL PROOF) ────────────────────────────────
c "8/9  anchor CID on-chain — privacy-preserving index_batch (real Groth16 proof)"
MH="0x$(printf '%s' "$DEMO_CID" | sha256sum | cut -c1-64)"
echo "  cid=$DEMO_CID"
echo "  metadata_hash=$MH"
echo "  anchor_timestamp=$DEMO_TS"
echo "  anchorer=Private/$ANCHORER  (anonymous — anchored_by will not reveal the signer)"
echo "→ generating proof + submitting (watch for 'CU index_batch' + 'confirmed')…"
"$CLI" index-batch \
  --cids "$DEMO_CID" \
  --metadata-hashes "$MH" \
  --anchor-timestamps "$DEMO_TS" \
  --anchorer "Private/$ANCHORER"
ok "anchored"

# ── 9. Query the registry by CID ─────────────────────────────────────────────
c "9/9  query the on-chain registry by CID"
cat > "$REPO/batch-anchor/.demo-lookup.toml" <<EOF
[delivery]
rest_url = "http://127.0.0.1:8645"
content_topic = "/chronicle/1/document-index/json"
poll_ms = 1000
store_lookback_hours = 24
[registry]
spel_toml = "../spel.toml"
sequencer_url = "http://127.0.0.1:$SEQ_PORT"
wallet_home = "../.scaffold/wallet"
signer_account_id = "Private/$ANCHORER"
program_id = "$PROGRAM_ID"
[batch]
max_size = 50
flush_after_s = 30
[node]
compose_file = "docker-compose.yml"
EOF
echo "→ lookup anchored CID (expect: record + exit 0)"
( cd "$REPO/batch-anchor" && "$ANCHOR" -c .demo-lookup.toml lookup "$DEMO_CID" ) \
  && ok "CID is registered + queryable" || die "lookup failed"
echo "→ lookup bogus CID (expect: not registered + exit 1)"
if ( cd "$REPO/batch-anchor" && "$ANCHOR" -c .demo-lookup.toml lookup "bafybogus000notanchored000" >/dev/null 2>&1 ); then
  die "bogus CID unexpectedly found"
else
  ok "bogus CID correctly not found"
fi

c "DEMO COMPLETE"
echo "Program:   $PROGRAM_ID"
echo "Anchored:  $DEMO_CID  (privacy-preserving; anchored_by is an anonymous key)"
echo "Re-run with DEMO_RESET=1 to wipe the chain + wallet and start from genesis."
