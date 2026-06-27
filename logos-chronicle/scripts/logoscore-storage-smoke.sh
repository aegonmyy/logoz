#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LOGOSCORE="${LOGOSCORE:-/nix/store/bsr21sfvfasl686mfi08r53g68pvc4gk-logos-logoscore-cli-bin-0.1.0/bin/logoscore}"
STORAGE_MODULES="${STORAGE_MODULES:-/nix/store/h5dqpgr89swrckr302rbyqipmsx558iq-logos-storage_module-module-lib-install/modules}"
CHRONICLE_MODULES="${CHRONICLE_MODULES:-/tmp/chronicle-next-install/modules}"

RUN_DIR="${RUN_DIR:-/tmp/chronicle-storage-smoke-$(date +%s)}"
LOG_DIR="$RUN_DIR/logoscore"
PERSIST_DIR="$RUN_DIR/persist"
STORAGE_DATA_DIR="$RUN_DIR/storage-data"
FILES_DIR="$RUN_DIR/files"
DAEMON_LOG="$RUN_DIR/daemon.log"
STORAGE_CONFIG="$RUN_DIR/storage-config.json"

mkdir -p "$LOG_DIR" "$PERSIST_DIR" "$STORAGE_DATA_DIR" "$FILES_DIR"

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
  # Post-cleanup audit: warn if anything tied to this run is still around.
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

printf '{"data-dir":"%s"}' "$STORAGE_DATA_DIR" >"$STORAGE_CONFIG"

STAMP="$(date +%s)"
SOURCE_FILE="$FILES_DIR/private-source-name-$STAMP.txt"
TITLE="Chronicle Storage Smoke $STAMP"
printf 'chronicle storage smoke %s\n' "$STAMP" >"$SOURCE_FILE"
SIZE_BYTES="$(wc -c <"$SOURCE_FILE" | tr -d ' ')"

"$LOGOSCORE" -D \
  --config-dir "$LOG_DIR" \
  --persistence-path "$PERSIST_DIR" \
  -m "$STORAGE_MODULES" \
  -m "$CHRONICLE_MODULES" \
  -v >"$DAEMON_LOG" 2>&1 &
# disown silences bash's "Aborted (core dumped)" job-control message at exit.
# The daemon hits SIGABRT during its own teardown (upstream bug in logoscore
# host); the test result is unaffected and cleanup_run still reaps it.
disown

sleep 1

"$LOGOSCORE" --config-dir "$LOG_DIR" load-module storage_module >/dev/null
"$LOGOSCORE" --config-dir "$LOG_DIR" call storage_module init @"$STORAGE_CONFIG" >/dev/null
"$LOGOSCORE" --config-dir "$LOG_DIR" call storage_module start >/dev/null
"$LOGOSCORE" --config-dir "$LOG_DIR" load-module chronicle >/dev/null

UPLOAD_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle uploadFileJson "$SOURCE_FILE" 'Text/Plain; charset=utf-8' "$TITLE")"
UPLOAD_ID="$(python3 -c '
import json
import sys

wrapper = json.loads(sys.stdin.read())
result = json.loads(wrapper["result"])
print(result["upload_id"])
' <<<"$UPLOAD_RESULT")"

if [[ -z "$UPLOAD_ID" ]]; then
  echo "Could not extract upload_id from result:" >&2
  echo "$UPLOAD_RESULT" >&2
  exit 1
fi

CID=""
STATUS=""
STATUS_RESULT=""
for _ in $(seq 1 60); do
  STATUS_RESULT="$("$LOGOSCORE" --config-dir "$LOG_DIR" call chronicle uploadStatusJson "$UPLOAD_ID")"
  read -r STATUS CID RESULT_SIZE < <(python3 -c '
import json
import sys

wrapper = json.loads(sys.stdin.read())
result = json.loads(wrapper["result"])
print(result.get("status", ""), result.get("cid", ""), result.get("size_bytes", ""))
' <<<"$STATUS_RESULT")

  if [[ "$STATUS" == "uploaded" ]]; then
    break
  fi
  if [[ "$STATUS" == "error" ]]; then
    echo "Upload failed:" >&2
    echo "$STATUS_RESULT" >&2
    exit 1
  fi
  sleep 2
done

if [[ "$STATUS" != "uploaded" || -z "$CID" ]]; then
  echo "Expected uploaded status with non-empty CID, got:" >&2
  echo "$STATUS_RESULT" >&2
  exit 1
fi

if [[ "$RESULT_SIZE" != "$SIZE_BYTES" ]]; then
  echo "Expected size_bytes=$SIZE_BYTES, got $RESULT_SIZE" >&2
  echo "$STATUS_RESULT" >&2
  exit 1
fi

STORED_LINES="$(grep 'Stored data' "$DAEMON_LOG" || true)"
SOURCE_BASENAME="$(basename "$SOURCE_FILE")"

if [[ -z "$STORED_LINES" ]]; then
  echo "Expected Storage daemon log to contain a Stored data line" >&2
  echo "Run dir: $RUN_DIR" >&2
  exit 1
fi

if ! printf '%s' "$STORED_LINES" | grep -q "$TITLE"; then
  echo "Expected Storage daemon log to mention title-derived filename/title: $TITLE" >&2
  echo "Run dir: $RUN_DIR" >&2
  exit 1
fi

if printf '%s' "$STORED_LINES" | grep -q "$SOURCE_BASENAME"; then
  echo "Storage Stored data line leaked source filename: $SOURCE_BASENAME" >&2
  echo "Run dir: $RUN_DIR" >&2
  exit 1
fi

echo "ok upload_id=$UPLOAD_ID cid=$CID run_dir=$RUN_DIR"
