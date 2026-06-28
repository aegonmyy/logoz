#!/usr/bin/env bash
# smoke: build a metadata envelope and broadcast it via Chronicle → Waku.
# Verifies: envelope built, broadcast_id returned, status reaches "sent",
# and Waku log lines confirm the message propagated.
# Uses the IT content topic from integration-test.toml so smoke traffic
# never reaches production /chronicle/1/... consumers.

set -euo pipefail

MODULE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LOGOSCORE="${LOGOSCORE:-/nix/store/bsr21sfvfasl686mfi08r53g68pvc4gk-logos-logoscore-cli-bin-0.1.0/bin/logoscore}"
STORAGE_MODS="${STORAGE_MODULES:-/nix/store/h5dqpgr89swrckr302rbyqipmsx558iq-logos-storage_module-module-lib-install/modules}"
DELIVERY_MODS="${DELIVERY_MODULES:-/nix/store/1wa7ryg2gplry809i6d2cxm0pq1yy2fi-logos-delivery_module-module-lib-install/modules}"
CHRONICLE_MODS="${CHRONICLE_MODULES:-/tmp/chronicle-next-install/modules}"

WORK_DIR="${WORK_DIR:-/tmp/wb-broadcast-smoke-$(date +%s)}"
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

[[ -d "$CHRONICLE_MODS" ]] || {
    echo "Chronicle modules not found: $CHRONICLE_MODS" >&2
    echo "Build first: nix build path:$MODULE_ROOT#install --out-link /tmp/chronicle-next-install" >&2
    exit 1
}

"$LOGOSCORE" -D \
    --config-dir "$LC_DIR" --persistence-path "$PER_DIR" \
    -m "$STORAGE_MODS" -m "$DELIVERY_MODS" -m "$CHRONICLE_MODS" \
    -v >"$DAEMON_LOG" 2>&1 &
disown

sleep 1

"$LOGOSCORE" --config-dir "$LC_DIR" load-module storage_module  >/dev/null
"$LOGOSCORE" --config-dir "$LC_DIR" load-module delivery_module >/dev/null
"$LOGOSCORE" --config-dir "$LC_DIR" load-module chronicle       >/dev/null

# Route smoke traffic to the IT topic
source "$(dirname "${BASH_SOURCE[0]}")/lib/load-integration-config.sh"
"$LOGOSCORE" --config-dir "$LC_DIR" call chronicle setBroadcastTopic "$IT_TOPIC" >/dev/null

"$LOGOSCORE" --config-dir "$LC_DIR" call chronicle startBroadcasterJson >/dev/null

TS="$(date +%s)"
FAKE_CID="bafywb-broadcast-smoke-${TS}"
ENV_INPUT="{\"cid\":\"${FAKE_CID}\",\"content_type\":\"text/plain\",\"size_bytes\":256,\"timestamp\":${TS},\"title\":\"WB Broadcast Smoke ${TS}\",\"description\":\"\",\"tags\":[\"wb\",\"smoke\"]}"

BUILD_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle buildMetadataEnvelopeJson "$ENV_INPUT")"
ENVELOPE="$(python3 -c '
import json, sys
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
assert res.get("ok") is not False, f"buildMetadataEnvelopeJson failed: {res}"
env  = res.get("envelope")
assert env, f"no envelope in: {res}"
print(json.dumps(env, separators=(",",":")))
' <<<"$BUILD_RESP")"
[[ -n "$ENVELOPE" ]] || { echo "Could not extract envelope from:" >&2; echo "$BUILD_RESP" >&2; exit 1; }

BCAST_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle broadcastEnvelopeJson "$ENVELOPE")"
BCAST_ID="$(python3 -c '
import json, sys
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
assert res.get("queued") == True, f"not queued: {res}"
bid  = res.get("broadcast_id", "")
assert bid, f"no broadcast_id in: {res}"
print(bid)
' <<<"$BCAST_RESP")"
[[ -n "$BCAST_ID" ]] || { echo "No broadcast_id in response:" >&2; echo "$BCAST_RESP" >&2; exit 1; }

sleep 12

STAT_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle broadcastStatusJson "$BCAST_ID")"
STATUS="$(python3 -c '
import json, sys
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
print(res.get("status",""))
' <<<"$STAT_RESP")"

[[ "$STATUS" == "sent" ]] || {
    echo "Expected status=sent; got status=$STATUS" >&2
    echo "$STAT_RESP" >&2
    exit 1
}

grep -q 'API send: scheduling delivery task' "$DAEMON_LOG"
grep -q 'Message successfully propagated'    "$DAEMON_LOG"
grep -q 'Message successfully sent'          "$DAEMON_LOG"

echo "ok broadcast_id=$BCAST_ID work_dir=$WORK_DIR"
