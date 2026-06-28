#!/usr/bin/env bash
# Bootstrap the Whistleblower workspace. Idempotent.
#
# Steps:
#   1. lgs setup          — initialise local lgs config
#   2. Sequencer probe    — start lgs localnet if port 3040 is dark
#   3. Build guest ELF    — only if the .bin is missing
#   4. Build host CLIs    — chronicle_registry_cli + batch-anchor
#   5. Deploy program     — idempotent (same ELF hash = same program_id)
#   6. Pin program_id     — write freshly-derived hash into batch-anchor.toml
#   7. Ensure signer      — mint a Public account if the pinned one isn't in wallet
#   8. Open registry      — batch-anchor init (idempotent)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

TOML="$REPO/batch-anchor/batch-anchor.toml"
WALLET="$REPO/.scaffold/wallet"
PROGRAM_BIN="$REPO/methods/guest/target/riscv32im-risc0-zkvm-elf/docker/chronicle_registry.bin"
CLI_BIN="$REPO/target/debug/chronicle_registry_cli"
ANCHOR_BIN="$REPO/batch-anchor/target/debug/batch-anchor"

cd "$REPO"

_tcp_open() { (exec 3<>/dev/tcp/127.0.0.1/3040) 2>/dev/null && exec 3<&-; }

echo "==> 1/8  lgs setup"
lgs setup

echo "==> 2/8  sequencer"
if _tcp_open; then
    echo "     already listening on 127.0.0.1:3040"
else
    echo "     starting localnet"
    lgs localnet start || true
    echo "     waiting for 127.0.0.1:3040 (up to 120 s)"
    for _i in $(seq 1 120); do
        _tcp_open && { echo "     up after ${_i}s"; break; }
        sleep 1
    done
    _tcp_open || {
        cat >&2 <<'ERR'
ERROR: port 3040 never came up within 120s.
The lgs 20s watchdog may have killed the sequencer while RocksDB was
recovering. Clear the cache and retry:
  rm -rf ~/.cache/logos-scaffold/repos/lez/*/rocksdb
  ./scripts/setup.sh
ERR
        exit 1
    }
fi

echo "==> 3/8  guest binary"
if [[ -f "$PROGRAM_BIN" ]]; then
    echo "     present — skipping build"
else
    echo "     building (this takes ~3 min)"
    make build
fi

echo "==> 4/8  host CLIs"
[[ -x "$CLI_BIN" ]]    || cargo build --bin chronicle_registry_cli
[[ -x "$ANCHOR_BIN" ]] || (cd "$REPO/batch-anchor" && cargo build --bin batch-anchor)

echo "==> 5/8  deploy program"
NSSA_WALLET_HOME_DIR="$WALLET" make deploy >/dev/null

echo "==> 6/8  pin program_id"
PROG_ID="$("$CLI_BIN" program-id "$PROGRAM_BIN" \
            | awk '/ImageID \(hex bytes\):/ {print $NF}')"
if [[ "$PROG_ID" =~ ^[a-f0-9]{64}$ ]]; then
    sed -i.bak -E "s|^program_id\s*=.*|program_id        = \"$PROG_ID\"|" "$TOML"
    rm -f "$TOML.bak"
    echo "     program_id=$PROG_ID"
else
    echo "WARN: could not parse program_id — $TOML left unchanged" >&2
fi

echo "==> 7/8  signer account"
PINNED="$(grep -E '^signer_account_id' "$TOML" | sed -E 's/.*"([^"]+)".*/\1/')"
if NSSA_WALLET_HOME_DIR="$WALLET" lgs wallet -- account list 2>/dev/null \
        | grep -qF "Public/$PINNED"; then
    echo "     $PINNED already in wallet"
else
    echo "     minting new Public account"
    NEW_ID="$(NSSA_WALLET_HOME_DIR="$WALLET" lgs wallet -- account new public 2>&1 \
                | sed -n 's|.*Public/\([A-Za-z0-9]\{32,\}\).*|\1|p' | head -1)"
    [[ -n "$NEW_ID" ]] || { echo "ERROR: failed to mint signer" >&2; exit 1; }
    echo "     minted $NEW_ID"
    sed -i.bak -E "s|^signer_account_id\s*=.*|signer_account_id = \"$NEW_ID\"|" "$TOML"
    rm -f "$TOML.bak"
fi

echo "==> 8/8  open registry"
(cd "$REPO/batch-anchor" && "$ANCHOR_BIN" init)

echo
echo "Done. To start the anchor watcher:"
echo "  cd batch-anchor && ./target/debug/batch-anchor watch"
