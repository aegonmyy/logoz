#!/usr/bin/env bash
# smoke: upload a file through Chronicle → Logos Storage.
# Verifies: upload_id returned, status reaches "uploaded", CID non-empty,
# size_bytes matches, and Storage daemon logs title-derived filename
# rather than the original source path.

set -euo pipefail

MODULE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LOGOSCORE="${LOGOSCORE:-/nix/store/bsr21sfvfasl686mfi08r53g68pvc4gk-logos-logoscore-cli-bin-0.1.0/bin/logoscore}"
STORAGE_MODS="${STORAGE_MODULES:-/nix/store/h5dqpgr89swrckr302rbyqipmsx558iq-logos-storage_module-module-lib-install/modules}"
CHRONICLE_MODS="${CHRONICLE_MODULES:-/tmp/chronicle-next-install/modules}"

WORK_DIR="${WORK_DIR:-/tmp/wb-storage-smoke-$(date +%s)}"
LC_DIR="$WORK_DIR/logoscore"
PER_DIR="$WORK_DIR/persist"
SD_DIR="$WORK_DIR/storage-data"
TMP_FILES="$WORK_DIR/files"
DAEMON_LOG="$WORK_DIR/daemon.log"
STOR_CFG="$WORK_DIR/storage.json"

mkdir -p "$LC_DIR" "$PER_DIR" "$SD_DIR" "$TMP_FILES"
printf '{"data-dir":"%s"}' "$SD_DIR" >"$STOR_CFG"

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

TS="$(date +%s)"
SRC="$TMP_FILES/private-upload-src-${TS}.txt"
TITLE="Storage Smoke ${TS}"
printf 'wb storage smoke payload %s\n' "$TS" >"$SRC"
FILE_BYTES="$(wc -c <"$SRC" | tr -d ' ')"

"$LOGOSCORE" -D \
    --config-dir "$LC_DIR" --persistence-path "$PER_DIR" \
    -m "$STORAGE_MODS" -m "$CHRONICLE_MODS" \
    -v >"$DAEMON_LOG" 2>&1 &
disown

sleep 1

"$LOGOSCORE" --config-dir "$LC_DIR" load-module storage_module >/dev/null
"$LOGOSCORE" --config-dir "$LC_DIR" call storage_module init @"$STOR_CFG" >/dev/null
"$LOGOSCORE" --config-dir "$LC_DIR" call storage_module start >/dev/null
"$LOGOSCORE" --config-dir "$LC_DIR" load-module chronicle >/dev/null

UPLOAD_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle uploadFileJson "$SRC" 'text/plain; charset=utf-8' "$TITLE")"

UPLOAD_ID="$(python3 -c '
import json, sys
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
assert res.get("ok") is not False,  f"uploadFileJson failed: {res}"
assert res.get("queued") == True,   f"queued not true: {res}"
uid = res.get("upload_id", "")
assert uid, f"no upload_id in: {res}"
print(uid)
' <<<"$UPLOAD_RESP")"

[[ -n "$UPLOAD_ID" ]] || { echo "No upload_id in response:" >&2; echo "$UPLOAD_RESP" >&2; exit 1; }

FINAL_STATUS=""
CID=""
STAT_RESP=""
for _ in $(seq 1 60); do
    STAT_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle uploadStatusJson "$UPLOAD_ID")"
    read -r FINAL_STATUS CID RET_BYTES < <(python3 -c '
import json, sys
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
print(res.get("status",""), res.get("cid",""), res.get("size_bytes",""))
' <<<"$STAT_RESP")
    [[ "$FINAL_STATUS" == "uploaded" ]] && break
    [[ "$FINAL_STATUS" == "error" ]]   && { echo "Upload errored:" >&2; echo "$STAT_RESP" >&2; exit 1; }
    sleep 2
done

[[ "$FINAL_STATUS" == "uploaded" && -n "$CID" ]] || {
    echo "Expected status=uploaded with non-empty cid; got status=$FINAL_STATUS cid=$CID" >&2
    echo "$STAT_RESP" >&2
    exit 1
}

[[ "$RET_BYTES" == "$FILE_BYTES" ]] || {
    echo "size_bytes mismatch: expected=$FILE_BYTES got=$RET_BYTES" >&2
    echo "$STAT_RESP" >&2
    exit 1
}

STORED="$(grep 'Stored data' "$DAEMON_LOG" || true)"
[[ -n "$STORED" ]] || {
    echo "FAIL: no 'Stored data' line in daemon log" >&2
    echo "work dir: $WORK_DIR" >&2
    exit 1
}
SRC_BASE="$(basename "$SRC")"
if printf '%s' "$STORED" | grep -q "$SRC_BASE"; then
    echo "FAIL: original source filename leaked into Storage log: $SRC_BASE" >&2
    printf '%s\n' "$STORED" >&2
    exit 1
fi

echo "ok upload_id=$UPLOAD_ID cid=$CID work_dir=$WORK_DIR"
