#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LOGOSCORE="${LOGOSCORE:-/nix/store/bsr21sfvfasl686mfi08r53g68pvc4gk-logos-logoscore-cli-bin-0.1.0/bin/logoscore}"
STORAGE_MODULES="${STORAGE_MODULES:-/nix/store/h5dqpgr89swrckr302rbyqipmsx558iq-logos-storage_module-module-lib-install/modules}"
DELIVERY_MODULES="${DELIVERY_MODULES:-/nix/store/1wa7ryg2gplry809i6d2cxm0pq1yy2fi-logos-delivery_module-module-lib-install/modules}"
CHRONICLE_MODULES="${CHRONICLE_MODULES:-/tmp/chronicle-next-install/modules}"

RUN_DIR="${RUN_DIR:-/tmp/chronicle-broadcast-smoke-$(date +%s)}"
LOG_DIR="$RUN_DIR/logoscore"
PERSIST_DIR="$RUN_DIR/persist"
DAEMON_LOG="$RUN_DIR/daemon.log"

mkdir -p "$LOG_DIR" "$PERSIST_DIR"

# Belt-and-braces cleanup. `logoscore stop` is the graceful path but it
# doesn't always reap the per-module host processes it spawned (esp. if a
# host is mid-syscall during Waku/storage init). Follow it with a pkill
# scoped to this run's RUN_DIR — every module host carries the path via
# --instance-persistence-path, so the match is surgical and won't touch
# unrelated logoscore/basecamp processes.
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

if [[ ! -d "$CHRONICLE_MODULES" ]]; then
  echo "Chronicle modules not found at $CHRONICLE_MODULES" >&2
  echo "Run: nix build path:$ROOT#install --out-link /tmp/chronicle-next-install" >&2
  exit 1
fi

"$LOGOSCORE" -D \
  --config-dir "$LOG_DIR" \
  --persistence-path "$PERSIST_DIR" \
  -m "$STORAGE_MODULES" \
  -m "$DELIVERY_MODULES" \
  -m "$CHRONICLE_MODULES" \
  -v >"$DAEMON_LOG" 2>&1 &
disown

sleep 1

"$LOGOSCORE" --config-dir "$LOG_DIR" load-module storage_module >/dev/null
"$LOGOSCORE" --config-dir "$LOG_DIR" load-module delivery_module >/dev/null
"$LOGOSCORE" --config-dir "$LOG_DIR" load-module chronicle >/dev/null

# Use the IT topic so smoke broadcasts never land on the production
# /chronicle/1/... topic — guards against test traffic leaking to live consumers.
source "$(dirname "${BASH_SOURCE[0]}")/lib/load-integration-config.sh"
"$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle setBroadcastTopic "$IT_TOPIC" >/dev/null

"$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle startBroadcasterJson >/dev/null

STAMP="$(date +%s)"
CID="bafychronicle-smoke-$STAMP"
INPUT="{\"cid\":\"$CID\",\"content_type\":\"text/plain\",\"size_bytes\":144,\"timestamp\":$STAMP,\"title\":\"Logoscore broadcast smoke\",\"description\":\"\",\"tags\":[\"logoscore\",\"smoke\"]}"

BUILD_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle buildMetadataEnvelopeJson "$INPUT")"
ENVELOPE="$(python3 -c '
import json
import sys

wrapper = json.loads(sys.stdin.read())
result = json.loads(wrapper["result"])
print(json.dumps(result["envelope"], separators=(",", ":")))
' <<<"$BUILD_RESULT")"
if [[ -z "$ENVELOPE" ]]; then
  echo "Could not extract envelope from build result:" >&2
  echo "$BUILD_RESULT" >&2
  exit 1
fi

BROADCAST_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle broadcastEnvelopeJson "$ENVELOPE")"
BROADCAST_ID="$(python3 -c '
import json
import sys

wrapper = json.loads(sys.stdin.read())
result = json.loads(wrapper["result"])
print(result["broadcast_id"])
' <<<"$BROADCAST_RESULT")"
if [[ -z "$BROADCAST_ID" ]]; then
  echo "Could not extract broadcast_id from result:" >&2
  echo "$BROADCAST_RESULT" >&2
  exit 1
fi

sleep 12

STATUS_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle broadcastStatusJson "$BROADCAST_ID")"
STATUS="$(python3 -c '
import json
import sys

wrapper = json.loads(sys.stdin.read())
result = json.loads(wrapper["result"])
print(result["status"])
' <<<"$STATUS_RESULT")"
if [[ "$STATUS" != "sent" ]]; then
  echo "Expected Chronicle status sent, got:" >&2
  echo "$STATUS_RESULT" >&2
  exit 1
fi

grep -q 'API send: scheduling delivery task' "$DAEMON_LOG"
grep -q 'Message successfully propagated' "$DAEMON_LOG"
grep -q 'Message successfully sent' "$DAEMON_LOG"

echo "ok broadcast_id=$BROADCAST_ID run_dir=$RUN_DIR"
