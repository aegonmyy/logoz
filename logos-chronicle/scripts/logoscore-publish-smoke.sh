#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LOGOSCORE="${LOGOSCORE:-/nix/store/bsr21sfvfasl686mfi08r53g68pvc4gk-logos-logoscore-cli-bin-0.1.0/bin/logoscore}"
STORAGE_MODULES="${STORAGE_MODULES:-/nix/store/h5dqpgr89swrckr302rbyqipmsx558iq-logos-storage_module-module-lib-install/modules}"
DELIVERY_MODULES="${DELIVERY_MODULES:-/nix/store/1wa7ryg2gplry809i6d2cxm0pq1yy2fi-logos-delivery_module-module-lib-install/modules}"
CHRONICLE_MODULES="${CHRONICLE_MODULES:-/tmp/chronicle-next-install/modules}"

RUN_DIR="${RUN_DIR:-/tmp/chronicle-publish-smoke-$(date +%s)}"
LOG_DIR="$RUN_DIR/logoscore"
PERSIST_DIR="$RUN_DIR/persist"
STORAGE_DATA_DIR="$RUN_DIR/storage-data"
DAEMON_LOG="$RUN_DIR/daemon.log"
STORAGE_CONFIG="$RUN_DIR/storage-config.json"

mkdir -p "$LOG_DIR" "$PERSIST_DIR" "$STORAGE_DATA_DIR"
printf '{"data-dir":"%s"}' "$STORAGE_DATA_DIR" >"$STORAGE_CONFIG"

# Belt-and-braces cleanup. `logoscore stop` is the graceful path but it
# doesn't always reap the per-module host processes it spawned (esp. if a
# host is mid-syscall during Waku/storage init). Follow it with a pkill
# scoped to this run's RUN_DIR — every module host carries the path via
# --instance-persistence-path, so the match is surgical and won't touch
# unrelated logoscore/basecamp processes. Covers both the first daemon
# (LOG_DIR) and the post-restart second daemon (LOG_DIR2) if set.
cleanup_run() {
  for cfg in "${LOG_DIR:-}" "${LOG_DIR2:-}"; do
    if [[ -n "$cfg" && -d "$cfg" ]]; then
      "$LOGOSCORE" --config-dir "$cfg" stop >/dev/null 2>&1 || true
    fi
  done
  pkill -TERM -f "$RUN_DIR" 2>/dev/null || true
  sleep 0.5
  pkill -KILL -f "$RUN_DIR" 2>/dev/null || true
  [[ -n "${SOURCE_FILE:-}" ]] && rm -f "$SOURCE_FILE"
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

# ---------------------------------------------------------------------------
# Create a temp source file with a name that must NOT appear in Storage logs.
# ---------------------------------------------------------------------------
SOURCE_FILE="$(mktemp /tmp/secret-source-XXXXXX.txt)"
echo "Chronicle publish smoke test content." >"$SOURCE_FILE"

SOURCE_BASENAME="$(basename "$SOURCE_FILE")"

# ---------------------------------------------------------------------------
# Start logoscore daemon
# ---------------------------------------------------------------------------
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
"$LOGOSCORE" --config-dir "$LOG_DIR" call storage_module init @"$STORAGE_CONFIG" >/dev/null
"$LOGOSCORE" --config-dir "$LOG_DIR" call storage_module start >/dev/null
"$LOGOSCORE" --config-dir "$LOG_DIR" load-module delivery_module >/dev/null
"$LOGOSCORE" --config-dir "$LOG_DIR" load-module chronicle >/dev/null

# Route this run's broadcasts onto the IT topic (see integration-test.toml).
source "$(dirname "${BASH_SOURCE[0]}")/lib/load-integration-config.sh"
"$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle setBroadcastTopic "$IT_TOPIC" >/dev/null

# Pre-initialise delivery node (delivery_module.start() return shape is only
# reliable when called synchronously from the logoscore CLI, not from an async
# timer callback inside the module).
"$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle startBroadcasterJson >/dev/null

# ---------------------------------------------------------------------------
# Publish
# ---------------------------------------------------------------------------
STAMP="$(date +%s)"
TITLE="PublishSmokeDoc-$STAMP"
REQUEST="{\"path\":\"$SOURCE_FILE\",\"content_type\":\"text/plain\",\"title\":\"$TITLE\",\"description\":\"smoke test\",\"tags\":[\"smoke\",\"publish\"],\"broadcast\":true}"

PUBLISH_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle publishFileJson "$REQUEST")"
PUBLISH_ID="$(python3 -c '
import json, sys
w = json.loads(sys.stdin.read())
r = json.loads(w["result"])
assert r.get("queued") == True, f"queued not true: {r}"
assert r.get("publish_id"), "no publish_id"
assert r.get("upload_id"), "no upload_id"
print(r["publish_id"])
' <<<"$PUBLISH_RESULT")"

if [[ -z "$PUBLISH_ID" ]]; then
  echo "publishFileJson did not return a publish_id:" >&2
  echo "$PUBLISH_RESULT" >&2
  exit 1
fi

echo "publish_id=$PUBLISH_ID"

# ---------------------------------------------------------------------------
# Poll publishStatusJson until broadcast_sent (max 120 s)
# ---------------------------------------------------------------------------
DEADLINE=$(( $(date +%s) + 120 ))
STATUS=""
while [[ $(date +%s) -lt $DEADLINE ]]; do
  STATUS_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle publishStatusJson "$PUBLISH_ID")"
  STATUS="$(python3 -c '
import json, sys
w = json.loads(sys.stdin.read())
r = json.loads(w["result"])
print(r.get("status",""))
' <<<"$STATUS_RESULT")"

  echo "  status=$STATUS"

  if [[ "$STATUS" == "broadcast_sent" ]]; then
    break
  fi
  if [[ "$STATUS" == "error" ]]; then
    echo "Publish errored:" >&2
    echo "$STATUS_RESULT" >&2
    exit 1
  fi
  sleep 3
done

if [[ "$STATUS" != "broadcast_sent" ]]; then
  echo "Timed out waiting for broadcast_sent; last status=$STATUS" >&2
  echo "$STATUS_RESULT" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# Verify fields present in terminal status
# ---------------------------------------------------------------------------
EXPECTED_ID="$PUBLISH_ID" python3 -c '
import json, sys, os
w = json.loads(sys.stdin.read())
r = json.loads(w["result"])
expected_publish_id = os.environ["EXPECTED_ID"]
assert r.get("ok") == True, f"ok not true: {r}"
assert r.get("publish_id") == expected_publish_id, "publish_id mismatch"
assert r.get("upload_id"), "no upload_id"
assert r.get("broadcast_id"), "no broadcast_id"
assert r.get("cid"), "no cid"
assert r.get("metadata_hash"), "no metadata_hash"
assert r.get("envelope"), "no envelope"
print("fields ok")
' <<<"$STATUS_RESULT"

# ---------------------------------------------------------------------------
# Confirm Storage stored the title-derived filename, not the original
# ---------------------------------------------------------------------------
STORED_LINES="$(grep 'Stored data' "$DAEMON_LOG" || true)"
if [[ -z "$STORED_LINES" ]]; then
  echo "Expected Storage daemon log to contain a 'Stored data' line" >&2
  exit 1
fi
if printf '%s' "$STORED_LINES" | grep -q "$SOURCE_BASENAME"; then
  echo "FAIL: original source filename leaked into Storage 'Stored data' log" >&2
  printf '%s\n' "$STORED_LINES" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# Confirm Delivery received the send (give network time to propagate)
# ---------------------------------------------------------------------------
sleep 12
grep -q 'API send: scheduling delivery task' "$DAEMON_LOG"
grep -q 'Message successfully propagated' "$DAEMON_LOG"
grep -q 'Message successfully sent' "$DAEMON_LOG"

echo "delivery ok"

# ---------------------------------------------------------------------------
# Restart and verify ledger persistence
# listPublishedJson reads from the in-memory map rebuilt from the ledger, so
# only Chronicle needs to be loaded — no Storage or Delivery required.
# ---------------------------------------------------------------------------
# Stop just the first daemon before swapping in the second on the same
# persistence dir. cleanup_run remains the EXIT trap and will catch both.
"$LOGOSCORE" --config-dir "$LOG_DIR" stop >/dev/null 2>&1 || true

LOG_DIR2="$RUN_DIR/logoscore2"
mkdir -p "$LOG_DIR2"

"$LOGOSCORE" -D \
  --config-dir "$LOG_DIR2" \
  --persistence-path "$PERSIST_DIR" \
  -m "$CHRONICLE_MODULES" \
  -v >>"$DAEMON_LOG" 2>&1 &
disown

sleep 1

"$LOGOSCORE" --config-dir "$LOG_DIR2" load-module chronicle >/dev/null

LIST_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR2" call chronicle listPublishedJson)"
EXPECTED_ID="$PUBLISH_ID" python3 -c '
import json, sys, os
expected_publish_id = os.environ["EXPECTED_ID"]
w = json.loads(sys.stdin.read())
r = json.loads(w["result"])
assert r.get("ok") == True, f"listPublishedJson not ok: {r}"
records = r.get("records", [])
assert len(records) >= 1, "no records returned after restart"
ids = [rec.get("publish_id") for rec in records]
assert expected_publish_id in ids, f"publish_id {expected_publish_id} not in list: {ids}"
match = next(rec for rec in records if rec["publish_id"] == expected_publish_id)
got = match.get("status"); assert got == "broadcast_sent", f"status after restart: {got}"
assert match.get("ok") == True, "ok not true after restart"
print(f"persistence ok: {len(records)} record(s) loaded")
' <<<"$LIST_RESULT"

echo "ok publish_id=$PUBLISH_ID run_dir=$RUN_DIR"
