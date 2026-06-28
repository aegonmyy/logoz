NSSA_WALLET_HOME_DIR="$PWD/.scaffold/wallet" \
  lgs wallet -- account get \
    --account-id "Public/9EE5bmzSBzojgFf5i11H8cTaXjXjfXvAAKb8yEpZ1its" --raw 2>&1 \
  | grep -E '^\{' | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
b = bytes.fromhex(d["data"]); i = 0
n = int.from_bytes(b[i:i+4], "little"); i += 4
print(f"entries: {n}")
for _ in range(n):
    klen = int.from_bytes(b[i:i+4], "little"); i += 4
    cid  = b[i:i+klen].decode(); i += klen
    mh   = b[i:i+32].hex();       i += 32
    ts   = int.from_bytes(b[i:i+8], "little", signed=True); i += 8
    by   = b[i:i+32].hex();       i += 32
    ver  = b[i]; i += 1
    print(f"  ts={ts} cid={cid} mhash=0x{mh[:16]}… v{ver}")'