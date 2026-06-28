#!/usr/bin/env bash
#
# Minimal bootstrap for the whistleblower workspace.  Idempotent.
# Handles the bits that need to happen on every fresh clone or
# sequencer reset: ensure a sequencer is up, build the guest, build the
# host-side binaries, deploy the program, ensure the configured signer
# exists, open the registry.
#
#   1. Run `lgs setup` — initialises the local lgs config that
#      `localnet start`, `wallet`, and `deploy` all depend on.
#      Idempotent: re-runs are a no-op on a configured machine.
#   2. Ensure the LEZ sequencer is running on 127.0.0.1:3040 — start
#      it via `lgs localnet start` if nothing is listening.
#   3. Build the guest binary (risc0 docker build) if it isn't built yet.
#   4. Build the host-side binaries (chronicle_registry_cli + batch-anchor)
#      if they aren't built yet.  batch-anchor shells out to
#      chronicle_registry_cli, and step 6 below invokes batch-anchor
#      directly — so both must exist before we get there.
#   5. Deploy chronicle-registry (idempotent on-chain: program_id is the
#      binary hash, so re-deploying the same binary is a no-op).
#   6. Mint a Public signer if the one pinned in batch-anchor.toml
#      isn't in the wallet; rewrite batch-anchor.toml to point at the
#      new ID.
#   7. Call `batch-anchor init` to open the registry (idempotent).
#
# Assumes the LEZ sequencer is already running (port 3040) and docker
# is up if you also want nwaku (`batch-anchor node up` is on you).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
PROD_TOML="$REPO_ROOT/batch-anchor/batch-anchor.toml"
WALLET_HOME="$REPO_ROOT/.scaffold/wallet"
PROGRAM_BIN="$REPO_ROOT/methods/guest/target/riscv32im-risc0-zkvm-elf/docker/chronicle_registry.bin"

cd "$REPO_ROOT"

CLI_BIN="$REPO_ROOT/target/debug/chronicle_registry_cli"
ANCHOR_BIN="$REPO_ROOT/batch-anchor/target/debug/batch-anchor"

# ── 1. Initialise lgs config ──────────────────────────────────────────
# Required on a fresh machine before localnet / wallet / deploy will
# work.  Idempotent on already-configured installs.
echo "→ lgs setup"
lgs setup

# ── 2. Ensure sequencer is up ─────────────────────────────────────────
# Probe port 3040 — start lgs localnet if nothing answers.
#
# `lgs localnet start` has a 20s readiness watchdog and may exit
# non-zero (killing the sequencer it spawned) if recovery takes longer
# than that — common when the cached rocksdb at
# ~/.cache/logos-scaffold/repos/lez/<sha>/rocksdb has lots of state.
# We tolerate that exit code and poll the port for up to 120s; if
# nothing binds in that window we bail with a clear message.
if (exec 3<>/dev/tcp/127.0.0.1/3040) 2>/dev/null; then
    exec 3<&-
    echo "→ Sequencer already running on 127.0.0.1:3040"
else
    echo "→ Start LEZ sequencer (lgs localnet start)"
    lgs localnet start || true
    echo "→ Waiting for 127.0.0.1:3040 (up to 120s)…"
    for i in $(seq 1 120); do
        if (exec 3<>/dev/tcp/127.0.0.1/3040) 2>/dev/null; then
            exec 3<&-
            echo "  bound after ${i}s"
            break
        fi
        sleep 1
    done
    if ! (exec 3<>/dev/tcp/127.0.0.1/3040) 2>/dev/null; then
        cat >&2 <<'EOF'

ERROR: sequencer never bound 127.0.0.1:3040 within 120s.

Most often this means lgs's 20s readiness watchdog killed the
sequencer before its RocksDB recovery finished.  If your local lgs
cache has accumulated state, try clearing it and re-running setup:

  rm -rf ~/.cache/logos-scaffold/repos/lez/*/rocksdb
  ./scripts/setup.sh

EOF
        exit 1
    fi
fi

# ── 3. Build guest if missing ─────────────────────────────────────────
if [[ ! -f "$PROGRAM_BIN" ]]; then
    echo "→ Build guest (cold; ~3 min)"
    make build
else
    echo "→ Guest binary present, skipping build"
fi

# ── 4. Build host-side binaries if missing ────────────────────────────
if [[ ! -x "$CLI_BIN" ]]; then
    echo "→ Build chronicle_registry_cli"
    cargo build --bin chronicle_registry_cli
fi
if [[ ! -x "$ANCHOR_BIN" ]]; then
    echo "→ Build batch-anchor"
    (cd "$REPO_ROOT/batch-anchor" && cargo build --bin batch-anchor)
fi

# ── 5. Deploy (idempotent — same binary hash = same program_id) ───────
echo "→ Deploy chronicle-registry"
NSSA_WALLET_HOME_DIR="$WALLET_HOME" make deploy >/dev/null

# Pin the freshly-built guest's program_id into batch-anchor.toml so a
# guest tweak doesn't leave the watcher pointing at the previous hash.
# Idempotent: same source → same ELF → same hash → sed is a no-op.
PROGRAM_ID=$("$CLI_BIN" program-id "$PROGRAM_BIN" \
    | awk '/ImageID \(hex bytes\):/ {print $NF}')
if [[ "$PROGRAM_ID" =~ ^[a-f0-9]{64}$ ]]; then
    sed -i.bak -E "s|^program_id\s*=.*|program_id        = \"$PROGRAM_ID\"|" "$PROD_TOML"
    rm -f "$PROD_TOML.bak"
    echo "  pinned program_id=$PROGRAM_ID"
else
    echo "WARN: could not parse program_id from $CLI_BIN output; batch-anchor.toml left untouched" >&2
fi

# ── 6. Ensure the pinned signer exists in the wallet ──────────────────
# On a fresh clone the pinned ID isn't in the wallet, so mint a new
# Public account and rewrite the prod toml. Test runs that need to
# scope to the IT topic do so via the BATCH_ANCHOR_DELIVERY__CONTENT_TOPIC
# env var, not a separate config file.
PINNED_SIGNER=$(grep -E '^signer_account_id' "$PROD_TOML" | sed -E 's/.*"([^"]+)".*/\1/')
echo "→ Verify signer $PINNED_SIGNER"
if NSSA_WALLET_HOME_DIR="$WALLET_HOME" lgs wallet -- account list 2>/dev/null \
     | grep -qF "Public/$PINNED_SIGNER"; then
    echo "  already in wallet"
else
    echo "  not in wallet — minting a new one"
    NEW_SIGNER=$(NSSA_WALLET_HOME_DIR="$WALLET_HOME" lgs wallet -- account new public 2>&1 \
        | sed -n 's|.*Public/\([A-Za-z0-9]\{32,\}\).*|\1|p' | head -1)
    [[ -n "$NEW_SIGNER" ]] || { echo "ERROR: could not mint signer" >&2; exit 1; }
    echo "  minted $NEW_SIGNER → rewriting batch-anchor.toml"
    sed -i.bak -E "s|^signer_account_id\s*=.*|signer_account_id = \"$NEW_SIGNER\"|" "$PROD_TOML"
    rm -f "$PROD_TOML.bak"
fi

# ── 7. Open the registry (idempotent) ─────────────────────────────────
echo "→ Init registry"
(cd "$REPO_ROOT/batch-anchor" && "$ANCHOR_BIN" init)

echo
echo "✅ Setup complete."
echo "   run anchor:  cd batch-anchor && ./target/debug/batch-anchor watch"
