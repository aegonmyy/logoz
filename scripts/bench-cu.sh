#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# LP-0017 — compute-unit benchmark for chronicle_registry::index_batch
#
# Measures the guest execution cycles (risc0_zkvm::guest::env::cycle_count()
# delta the guest logs as `CU index_batch n=<k> cycles=<n>`) for anchoring a
# batch of N CIDs into a FRESH registry, against a real local rc5 sequencer at
# RISC0_DEV_MODE=0. The CU line is emitted by the guest during r0vm execution,
# so it is captured from the sequencer log, not the CLI stdout.
#
# The cycle count is a property of guest execution (the executor runs whether
# or not a Groth16 proof is produced), so it is independent of RISC0_DEV_MODE;
# we still run at DEV_MODE=0 so every measured tx is a real proof. The n=1 point
# cross-checks against the 2809 value captured on-camera in scripts/demo.sh.
#
# Prereq: run `DEMO_RESET=1 ./scripts/demo.sh` once first so the wallet + guest
# binary + toolchain exist. This script re-wipes the chain per batch size so
# each N is measured against a freshly-initialised (empty) registry.
#
# Usage:  ./scripts/bench-cu.sh            # measures n=1 and n=50
#         SIZES="1 10 25 50" ./scripts/bench-cu.sh
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

WALLET_DIR="$REPO/.scaffold/wallet"
: "${LOGOS_SCAFFOLD_WALLET_PASSWORD:=logosdemo50}"
export LOGOS_SCAFFOLD_WALLET_PASSWORD
export LEE_WALLET_HOME_DIR="$WALLET_DIR"
GUEST_BIN="$REPO/methods/guest/target/riscv32im-risc0-zkvm-elf/docker/chronicle_registry.bin"
CLI="$REPO/target/debug/chronicle_registry_cli"
SEQ_PORT=3040
SEQ_LOG="$REPO/.scaffold/logs/sequencer.log"
SIZES="${SIZES:-1 50}"
export RISC0_DEV_MODE=0

say() { printf '\n\033[1;36m══ %s\033[0m\n' "$*"; }
ok()  { printf '\033[1;32m✓ %s\033[0m\n' "$*"; }
die() { printf '\033[1;31m✗ %s\033[0m\n' "$*" >&2; exit 1; }

lez_cache() {
  local rev
  rev="$(grep -E '^\s*pin\s*=' "$REPO/scaffold.toml" | head -1 | sed -E 's/.*"([0-9a-f]+)".*/\1/')"
  echo "$HOME/.cache/logos-scaffold/repos/lez/$rev"
}

# ── setup (mirror demo.sh phases 1-4) ────────────────────────────────────────
say "setup — toolchain, config bridge, guest, wallet (RISC0_DEV_MODE=0)"
lgs setup >/dev/null
SPEL_BIN="$(find "$HOME/.cache/logos-scaffold/repos/spel" -path '*/target/release/spel' 2>/dev/null | head -1)"
[ -n "$SPEL_BIN" ] && ln -sf "$SPEL_BIN" "$HOME/.cargo/bin/spel" 2>/dev/null || true
LEZ="$(lez_cache)"
[ -d "$LEZ/lez/sequencer" ] && ln -sfn lez/sequencer "$LEZ/sequencer"
[ -d "$LEZ/lez/wallet" ]    && ln -sfn lez/wallet    "$LEZ/wallet"
[ -f "$GUEST_BIN" ] || make build
[ -x "$CLI" ] || cargo build --bin chronicle_registry_cli >/dev/null
WALLET_BIN="$LEZ/target/release/wallet"
"$WALLET_BIN" account list >/dev/null 2>&1 || true
mapfile -t ACCTS < <("$WALLET_BIN" account list 2>/dev/null | sed -nE 's#^/ (Public|Private)/([1-9A-HJ-NP-Za-km-z]+).*#\1 \2#p')
DEPLOYER="$(printf '%s\n' "${ACCTS[@]}" | awk '$1=="Public"{print $2; exit}')"
[ -n "$DEPLOYER" ] || die "no Public deployer in wallet — run 'DEMO_RESET=1 ./scripts/demo.sh' once first"
SEQ_CFG="$LEZ/lez/sequencer/service/configs/debug/sequencer_config.json"
ok "deployer Public/$DEPLOYER"

# ── fresh chain + freshly-opened registry ────────────────────────────────────
# Wipe rocksdb, restart, fund the Public deployer via the faucet, deploy the
# program and open a brand-new (empty) registry PDA. Each batch-size measurement
# starts from this clean state so the reported cycles reflect anchoring N CIDs
# into an empty registry (not one already carrying entries from a prior batch).
fresh_chain() {
  python3 - "$SEQ_CFG" "$DEPLOYER" <<'PY'
import json, sys
p, dep = sys.argv[1], sys.argv[2]
d = json.load(open(p)); g = d["genesis"]
ids = [list(e.values())[0].get("account_id") for e in g if "supply_account" in e]
if dep not in ids:
    g.append({"supply_account": {"account_id": dep, "balance": 1000000}})
    json.dump(d, open(p, "w"), indent=2)
PY
  lgs localnet stop >/dev/null 2>&1 || true
  rm -rf "$LEZ/rocksdb"
  rm -f "$REPO/.scaffold/state/sequencer_config.json"
  lgs localnet start --timeout-sec 90 >/dev/null
  (exec 3<>"/dev/tcp/127.0.0.1/$SEQ_PORT") 2>/dev/null && exec 3<&- || die "sequencer not up on :$SEQ_PORT"
  "$WALLET_BIN" auth-transfer init --account-id "Public/$DEPLOYER" >/dev/null 2>&1 || true
  lgs wallet topup "Public/$DEPLOYER" >/dev/null 2>&1 || true
  local bal
  bal="$("$WALLET_BIN" account get --account-id "Public/$DEPLOYER" 2>/dev/null | sed -n 's/.*"balance":\([0-9]*\).*/\1/p')"
  [ "${bal:-0}" -gt 0 ] || die "faucet did not fund the deployer (balance ${bal:-0})"
  lgs wallet default set "Public/$DEPLOYER" >/dev/null 2>&1 || true
  lgs deploy --program-path "$GUEST_BIN" >/dev/null
  "$CLI" init-registry --anchorer "Public/$DEPLOYER" >/dev/null 2>&1 || true
}

# ── anchor N CIDs and extract the guest CU line from the sequencer log ────────
bench_n() {
  local N="$1" base i
  base="$(date +%s)"
  local cids=() hashes=() ts=()
  for ((i=0; i<N; i++)); do
    cids+=("bafybenchcu$(printf '%04d' "$i")t${base}")
    hashes+=("0x$(printf 'benchcu%04d%s' "$i" "$base" | sha256sum | cut -c1-64)")
    ts+=("$((base + i))")
  done
  local CIDS HASHES TSS
  CIDS="$(IFS=,; echo "${cids[*]}")"
  HASHES="$(IFS=,; echo "${hashes[*]}")"
  TSS="$(IFS=,; echo "${ts[*]}")"

  local mark
  mark="$(wc -l < "$SEQ_LOG" 2>/dev/null || echo 0)"
  "$CLI" index-batch \
    --cids "$CIDS" \
    --metadata-hashes "$HASHES" \
    --anchor-timestamps "$TSS" \
    --anchorer "Public/$DEPLOYER" >/dev/null

  local line=""
  for _ in $(seq 1 30); do
    line="$(tail -n +"$((mark + 1))" "$SEQ_LOG" 2>/dev/null \
            | grep -oE "CU index_batch n=$N cycles=[0-9]+" | tail -1)"
    [ -n "$line" ] && break
    sleep 1
  done
  if [ -z "$line" ]; then
    echo "  (no CU line captured for n=$N — recent sequencer log tail:)" >&2
    tail -n +"$((mark + 1))" "$SEQ_LOG" 2>/dev/null | tail -20 >&2
    echo "CU index_batch n=$N cycles=UNKNOWN"
    return 0
  fi
  echo "$line"
}

RESULTS=()
FAILED=0
for N in $SIZES; do
  say "benchmark n=$N (fresh registry, real proof unless localnet dev-mode)"
  fresh_chain
  R="$(bench_n "$N")"
  ok "$R"
  RESULTS+=("$R")
  case "$R" in *UNKNOWN*) FAILED=1 ;; esac
done

say "RESULTS"
printf '%s\n' "${RESULTS[@]}"

# Fail loudly if any batch failed to anchor / log its CU line. This also makes
# the script usable as a CI end-to-end gate for the on-chain anchor pipeline.
[ "$FAILED" -eq 0 ] || die "one or more batches did not anchor (no CU line captured)"
