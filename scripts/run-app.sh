#!/usr/bin/env bash
# Launch the Whistleblower desktop app.
#
# Usage: ./scripts/run-app.sh [extra basecamp args]
#
# What this does (in order):
#   1. setup.sh — build, deploy, mint signer, open registry (idempotent)
#   2. nix build chronicle + whistleblower modules
#   3. nix build upstream storage + delivery modules
#   4. nix build pinned basecamp (0.1.2-RC1)
#   5. Stage modules into .basecamp-data/ (project-local, won't touch ~/.local)
#   6. Start nwaku + batch-anchor watch daemon in background
#   7. exec basecamp (this terminal becomes basecamp's stdout)
#
# User state (module_data, anchor config, logs) lives in .basecamp-data/
# and survives re-runs. Delete it to start fresh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO"

# Pinned to 0.1.2-RC1 — newer revs hit a liblogos_core shutdown crash
BASECAMP_REV="064ef3f168a061c77f8cc1bb6afc9f0a04f5b920"
BASECAMP_NIX="github:logos-co/logos-basecamp/${BASECAMP_REV}"

DATA_DIR="$REPO/.basecamp-data"
ANCHOR_BIN="$REPO/batch-anchor/target/debug/batch-anchor"
ANCHOR_LOG="$DATA_DIR/logs/batch-anchor.log"

# ── 1. On-chain bootstrap ─────────────────────────────────────────────────────
echo ">>> [1/6] on-chain setup"
"$SCRIPT_DIR/setup.sh"

# ── 2 + 3. Build Logos modules ────────────────────────────────────────────────
echo
echo ">>> [2/6] build modules"
nix build .#chronicle-install     -o /tmp/wb-chronicle-install
nix build .#whistleblower-install -o /tmp/wb-whistleblower-install
nix build 'github:logos-co/logos-storage-module#install'  -o /tmp/wb-storage-install
nix build 'github:logos-co/logos-delivery-module#install' -o /tmp/wb-delivery-install

# ── 3. Pinned basecamp ────────────────────────────────────────────────────────
echo
echo ">>> [3/6] fetch basecamp (${BASECAMP_REV:0:8})"
nix build "$BASECAMP_NIX" -o /tmp/wb-basecamp

# ── 4. Stage into project-local data dir ─────────────────────────────────────
echo
echo ">>> [4/6] stage modules → $DATA_DIR"
# nix-store paths are 0555; remove and recreate to avoid "permission denied"
# on repeated runs. User state dirs are not touched.
chmod -R u+w "$DATA_DIR/modules" "$DATA_DIR/plugins" 2>/dev/null || true
rm -rf "$DATA_DIR/modules" "$DATA_DIR/plugins"
mkdir -p "$DATA_DIR/modules" "$DATA_DIR/plugins"

cp -rf /tmp/wb-storage-install/modules/.      "$DATA_DIR/modules/"
cp -rf /tmp/wb-delivery-install/modules/.     "$DATA_DIR/modules/"
cp -rf /tmp/wb-chronicle-install/modules/.    "$DATA_DIR/modules/"
cp -rf /tmp/wb-whistleblower-install/plugins/. "$DATA_DIR/plugins/"

# ── 5. Start nwaku + batch-anchor watcher ────────────────────────────────────
echo
echo ">>> [5/6] start batch-anchor (nwaku + watcher)"
mkdir -p "$DATA_DIR/logs"

[[ -x "$ANCHOR_BIN" ]] || (cd "$REPO/batch-anchor" && cargo build --bin batch-anchor)

(cd "$REPO/batch-anchor" && "$ANCHOR_BIN" node up >/dev/null)

# Wait for nwaku readiness before the watcher's first poll
for _ in {1..15}; do
    curl -sf --max-time 1 http://127.0.0.1:8645/health 2>/dev/null | grep -q READY && break
    sleep 1
done

# Kill any stale watcher from a previous run
pkill -f "batch-anchor.*watch" 2>/dev/null && sleep 1 || true

(
    cd "$REPO/batch-anchor"
    RISC0_DEV_MODE=0 RUST_LOG=batch_anchor=info nohup "$ANCHOR_BIN" watch \
        >"$ANCHOR_LOG" 2>&1 </dev/null &
    disown
)
echo "     watcher running → tail -f $ANCHOR_LOG"

# ── 6. Launch basecamp ────────────────────────────────────────────────────────
echo
echo ">>> [6/6] launching basecamp"
# RISC0_DEV_MODE=0 forces full ZK proof generation — required by LP-0017
# evaluation criteria. Terminal output will show proof cycles, confirming
# this is not a dev-mode shortcut.
export RISC0_DEV_MODE=0
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
exec /tmp/wb-basecamp/bin/LogosBasecamp --user-dir "$DATA_DIR" "$@"
