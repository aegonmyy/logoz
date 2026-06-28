#!/usr/bin/env bash
#
# One-shot launcher for the LP-17 Whistleblower app.
#
#   git clone <repo>
#   cd whistleblower
#   ./scripts/run-app.sh
#
# Assumes:
#   - LEZ sequencer is already running (`lgs localnet start`).
#   - .scaffold/wallet exists (created by lgs scaffolding or carried over).
#
# Does, in order:
#   1. scripts/setup.sh — build + deploy chronicle-registry guest, mint
#      signer, open the registry. Idempotent; cheap on re-run.
#   2. nix build chronicle + whistleblower (ours) and storage + delivery
#      (upstream, locked via flake.lock).
#   3. nix build a pinned basecamp (0.1.2-RC1 — newer revs hit an upstream
#      liblogos_core shutdown crash; see logos-whistleblower/README.md).
#   4. Stage everything into a project-local data dir
#      ($REPO_ROOT/.basecamp-data). We pass it to basecamp via --user-dir
#      so we never touch the user's regular ~/.local/share/Logos/.
#   5. Bring up nwaku + start `batch-anchor watch` in the background.
#      Replaces any previous watcher so re-runs don't stack.
#   6. Launch basecamp, forcing xcb on WSL/Linux without Wayland.
#
# The project-local data dir means re-running is non-destructive to any
# basecamp the user already has set up elsewhere. Nuke .basecamp-data/
# to start fresh.
#
# Where output goes:
#   - basecamp:       this terminal (foreground via `exec`)
#   - batch-anchor:   $DATA_DIR/logs/batch-anchor.log (background daemon)
#   - nwaku:          docker logs chronicle-nwaku
#   - sequencer:      wherever you started `lgs localnet start` (external)
# When you Ctrl-C basecamp, the batch-anchor daemon keeps running.  Stop
# it manually with `pkill -f 'batch-anchor.*watch'` and `batch-anchor
# node down` if you want a clean shutdown.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# Pinned basecamp rev — 0.1.2-RC1. Bumping this needs end-to-end re-test.
BASECAMP_REV="064ef3f168a061c77f8cc1bb6afc9f0a04f5b920"
BASECAMP_URL="github:logos-co/logos-basecamp/${BASECAMP_REV}"

# Project-local data dir — basecamp's --user-dir flag isolates plugins,
# modules, module_data, logs here. The user's ~/.local/share/Logos is
# untouched.
DATA_DIR="$REPO_ROOT/.basecamp-data"

# ── 1. On-chain setup ───────────────────────────────────────────────────
echo "▸ Step 1/6: on-chain setup"
"$SCRIPT_DIR/setup.sh"

# ── 2. Build modules ────────────────────────────────────────────────────
echo
echo "▸ Step 2/6: build modules"
nix build .#chronicle-install                  -o /tmp/chronicle-install
nix build .#whistleblower-install              -o /tmp/whistleblower-install
nix build 'github:logos-co/logos-storage-module#install'  -o /tmp/storage-install
nix build 'github:logos-co/logos-delivery-module#install' -o /tmp/delivery-install

# ── 3. Build pinned basecamp ────────────────────────────────────────────
echo
echo "▸ Step 3/6: fetch basecamp (${BASECAMP_REV:0:7})"
nix build "$BASECAMP_URL" -o /tmp/logos-basecamp

# ── 4. Stage everything into the project-local data dir ─────────────────
echo
echo "▸ Step 4/6: stage modules into $DATA_DIR"
# Wipe modules/ and plugins/ before re-staging. nix-store .so files
# come with mode 0555, so a cp -rf rerun can't unlink them; just
# clearing the dirs sidesteps the whole permissions dance. User state
# (module_data/, logs/, chronicle/anchor-config.json) lives elsewhere
# in $DATA_DIR and is preserved.
chmod -R u+w "$DATA_DIR/modules" "$DATA_DIR/plugins" 2>/dev/null || true
rm -rf "$DATA_DIR/modules" "$DATA_DIR/plugins"
mkdir -p "$DATA_DIR/modules" "$DATA_DIR/plugins"
cp -rf /tmp/storage-install/modules/.         "$DATA_DIR/modules/"
cp -rf /tmp/delivery-install/modules/.        "$DATA_DIR/modules/"
cp -rf /tmp/chronicle-install/modules/.       "$DATA_DIR/modules/"
cp -rf /tmp/whistleblower-install/plugins/.   "$DATA_DIR/plugins/"

# ── 5. Bring up nwaku + batch-anchor watch ──────────────────────────────
echo
echo "▸ Step 5/6: start batch-anchor (nwaku + watch daemon)"
mkdir -p "$DATA_DIR/logs"
ANCHOR_LOG="$DATA_DIR/logs/batch-anchor.log"
ANCHOR_BIN="$REPO_ROOT/batch-anchor/target/debug/batch-anchor"

if [[ ! -x "$ANCHOR_BIN" ]]; then
    echo "  building batch-anchor (one-time)"
    (cd "$REPO_ROOT/batch-anchor" && cargo build --bin batch-anchor)
fi

# Nwaku via docker compose — idempotent.
(cd "$REPO_ROOT/batch-anchor" && "$ANCHOR_BIN" node up >/dev/null)

# Wait for nwaku readiness so the watcher doesn't crash on its first poll.
for _ in {1..15}; do
    curl -sf --max-time 1 http://127.0.0.1:8645/health 2>/dev/null | grep -q READY && break
    sleep 1
done

# Replace any prior watcher so re-running this script doesn't pile them up.
pkill -f "batch-anchor.*watch" 2>/dev/null && sleep 1 || true

# Launch the anchor in the background. nohup + redirected fds + setsid-by-disown
# means it survives this script's `exec` to basecamp.
(
    cd "$REPO_ROOT/batch-anchor"
    RUST_LOG=batch_anchor=info nohup "$ANCHOR_BIN" watch \
        > "$ANCHOR_LOG" 2>&1 < /dev/null &
    disown
)
echo "  anchor running in background → tail -f $ANCHOR_LOG"

# ── 6. Launch ───────────────────────────────────────────────────────────
echo
echo "▸ Step 6/6: launch basecamp (data dir: $DATA_DIR)"
# Force xcb by default. WSL sets WAYLAND_DISPLAY but Qt's wayland plugin
# can't actually open a display there; native Linux desktops with working
# Wayland still ship xcb as a fallback, so this is safe. Honor a pre-set
# value so users on a real Wayland session can override.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
exec /tmp/logos-basecamp/bin/LogosBasecamp --user-dir "$DATA_DIR" "$@"
