#!/usr/bin/env bash
# smoke: upload a file + broadcast its envelope via publishFileJson.
# Verifies: publish_id returned, status reaches "done",
# CID/metadata_hash/envelope present, Storage logs title not source path,
# Waku confirms delivery, and the publish record survives a daemon restart.

set -euo pipefail

MODULE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LOGOSCORE="${LOGOSCORE:-/nix/store/bsr21sfvfasl686mfi08r53g68pvc4gk-logos-logoscore-cli-bin-0.1.0/bin/logoscore}"
STORAGE_MODS="${STORAGE_MODULES:-/nix/store/h5dqpgr89swrckr302rbyqipmsx558iq-logos-storage_module-module-lib-install/modules}"
DELIVERY_MODS="${DELIVERY_MODULES:-/nix/store/1wa7ryg2gplry809i6d2cxm0pq1yy2fi-logos-delivery_module-module-lib-install/modules}"
CHRONICLE_MODS="${CHRONICLE_MODULES:-/tmp/chronicle-next-install/modules}"

WORK_DIR="${WORK_DIR:-/tmp/wb-publish-smoke-$(date +%s)}"
LC_DIR="$WORK_DIR/logoscore"
LC_DIR2=""
PER_DIR="$WORK_DIR/persist"
SD_DIR="$WORK_DIR/storage-data"
DAEMON_LOG="$WORK_DIR/daemon.log"
STOR_CFG="$WORK_DIR/storage.json"
SRC_FILE=""

mkdir -p "$LC_DIR" "$PER_DIR" "$SD_DIR"
printf '{"data-dir":"%s"}' "$SD_DIR" >"$STOR_CFG"

_stop_all() {
    for cfg in "${LC_DIR:-}" "${LC_DIR2:-}"; do
        [[ -n "$cfg" && -d "$cfg" ]] && "$LOGOSCORE" --config-dir "$cfg" stop >/dev/null 2>&1 || true
    done
    pkill -TERM -f "$WORK_DIR" 2>/dev/null || true
    sleep 0.5
    pkill -KILL -f "$WORK_DIR" 2>/dev/null || true
    [[ -n "${SRC_FILE:-}" ]] && rm -f "$SRC_FILE"
    local rem
    rem="$(pgrep -af "$WORK_DIR" | grep -v "pgrep -af" || true)"
    if [[ -n "$rem" ]]; then
        echo "WARN: lingering processes tied to $WORK_DIR:" >&2
        echo "$rem" >&2
    fi
    # Never let the EXIT trap's last statement decide the script's exit code:
    # a bare `[[ -n "$rem" ]] && …` returns 1 on the clean (no-lingering) path,
    # which would fail an otherwise-successful smoke run.
    return 0
}
trap _stop_all EXIT INT TERM

[[ -d "$CHRONICLE_MODS" ]] || {
    echo "Chronicle modules not found: $CHRONICLE_MODS" >&2
    echo "Build first: nix build path:$MODULE_ROOT#install --out-link /tmp/chronicle-next-install" >&2
    exit 1
}

# Source file intentionally has a name that must not appear in Storage logs
SRC_FILE="$(mktemp /tmp/confidential-src-XXXXXX.txt)"
printf 'wb publish smoke payload %s\n' "$(date +%s)" >"$SRC_FILE"
SRC_BASE="$(basename "$SRC_FILE")"

"$LOGOSCORE" -D \
    --config-dir "$LC_DIR" --persistence-path "$PER_DIR" \
    -m "$STORAGE_MODS" -m "$DELIVERY_MODS" -m "$CHRONICLE_MODS" \
    -v >"$DAEMON_LOG" 2>&1 &
disown

sleep 1

"$LOGOSCORE" --config-dir "$LC_DIR" load-module storage_module  >/dev/null
"$LOGOSCORE" --config-dir "$LC_DIR" call storage_module init @"$STOR_CFG" >/dev/null
"$LOGOSCORE" --config-dir "$LC_DIR" call storage_module start   >/dev/null
"$LOGOSCORE" --config-dir "$LC_DIR" load-module delivery_module >/dev/null
"$LOGOSCORE" --config-dir "$LC_DIR" load-module chronicle       >/dev/null

source "$(dirname "${BASH_SOURCE[0]}")/lib/load-integration-config.sh"
"$LOGOSCORE" --config-dir "$LC_DIR" call chronicle setBroadcastTopic "$IT_TOPIC" >/dev/null
"$LOGOSCORE" --config-dir "$LC_DIR" call chronicle startBroadcasterJson          >/dev/null

TS="$(date +%s)"
TITLE="WB Publish Smoke ${TS}"
REQ="{\"path\":\"$SRC_FILE\",\"content_type\":\"text/plain\",\"title\":\"$TITLE\",\"description\":\"smoke\",\"tags\":[\"wb\",\"smoke\"],\"broadcast\":true}"

PUB_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle publishFileJson "$REQ")"
PUB_ID="$(python3 -c '
import json, sys
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
assert res.get("queued") == True, f"not queued: {res}"
pid  = res.get("publish_id", "")
assert pid, f"no publish_id in: {res}"
print(pid)
' <<<"$PUB_RESP")"

[[ -n "$PUB_ID" ]] || { echo "No publish_id in response:" >&2; echo "$PUB_RESP" >&2; exit 1; }
echo "publish_id=$PUB_ID"

DEAD=$(( $(date +%s) + 120 ))
FINAL=""
STAT_RESP=""
while [[ $(date +%s) -lt $DEAD ]]; do
    STAT_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR" call chronicle publishStatusJson "$PUB_ID")"
    FINAL="$(python3 -c '
import json, sys
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
print(res.get("status",""))
' <<<"$STAT_RESP")"
    echo "  status=$FINAL"
    [[ "$FINAL" == "done"  ]] && break
    [[ "$FINAL" == "error" ]] && { echo "Publish errored:" >&2; echo "$STAT_RESP" >&2; exit 1; }
    sleep 3
done

[[ "$FINAL" == "done" ]] || {
    echo "Timed out; last status=$FINAL" >&2
    echo "$STAT_RESP" >&2
    exit 1
}

EXPECTED_ID="$PUB_ID" python3 -c '
import json, sys, os
wrap = json.loads(sys.stdin.read())
res  = json.loads(wrap["result"])
eid  = os.environ["EXPECTED_ID"]
assert res.get("ok")         is not False, f"ok missing: {res}"
assert res.get("publish_id") == eid,       f"publish_id mismatch"
assert res.get("cid"),                     f"no cid"
assert res.get("metadata_hash"),           f"no metadata_hash"
print("fields ok")
' <<<"$STAT_RESP"

STORED="$(grep 'Stored data' "$DAEMON_LOG" || true)"
[[ -n "$STORED" ]] || { echo "No 'Stored data' line in daemon log" >&2; exit 1; }
if printf '%s' "$STORED" | grep -q "$SRC_BASE"; then
    echo "FAIL: original source filename leaked into Storage log: $SRC_BASE" >&2
    exit 1
fi

sleep 12
grep -q 'API send: scheduling delivery task' "$DAEMON_LOG"
grep -q 'Message successfully propagated'    "$DAEMON_LOG"
grep -q 'Message successfully sent'          "$DAEMON_LOG"
echo "delivery ok"

# Restart and verify the publish record is in the ledger
"$LOGOSCORE" --config-dir "$LC_DIR" stop >/dev/null 2>&1 || true
LC_DIR2="$WORK_DIR/logoscore2"
mkdir -p "$LC_DIR2"

"$LOGOSCORE" -D \
    --config-dir "$LC_DIR2" --persistence-path "$PER_DIR" \
    -m "$CHRONICLE_MODS" -v >>"$DAEMON_LOG" 2>&1 &
disown

sleep 1

"$LOGOSCORE" --config-dir "$LC_DIR2" load-module chronicle >/dev/null

LIST_RESP="$("$LOGOSCORE" --config-dir "$LC_DIR2" call chronicle listPublishedJson)"
EXPECTED_ID="$PUB_ID" python3 -c '
import json, sys, os
wrap  = json.loads(sys.stdin.read())
res   = json.loads(wrap["result"])
eid   = os.environ["EXPECTED_ID"]
assert res.get("ok") is not False, f"listPublishedJson failed: {res}"
items = res.get("items", [])
assert len(items) >= 1, f"no items after restart"
ids   = [it.get("publish_id") for it in items]
assert eid in ids, f"{eid} not in {ids}"
match = next(it for it in items if it["publish_id"] == eid)
st = match.get("status")
assert st == "done", f"status after restart: {st}"
print(f"persistence ok ({len(items)} record(s))")
' <<<"$LIST_RESP"

echo "ok publish_id=$PUB_ID work_dir=$WORK_DIR"
