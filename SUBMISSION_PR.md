# LP-0017 Whistleblower — Submission

**Prize**: LP-0017 · $400 USDC · First-come-first-served  
**Repo**: https://github.com/aegonmyy/logoz  
**Submitter**: aegonmyy

---

## What was built

A complete censorship-resistant document publishing pipeline on Logos, delivered as two Logos modules and a background anchor daemon:

```
User picks file
      │
      ▼
logos-chronicle (Qt plugin)
  ├─ Upload    → Logos Storage (Codex)  → CID
  ├─ Broadcast → Logos Delivery (Waku)  → /chronicle/1/document-index/json
  └─ Anchor    → LEZ SPEL program       → on-chain CidRecord
      │
      ▼
batch-anchor (background daemon)
  ├─ Drains Waku store for missed envelopes
  ├─ Deduplicates by (CID, metadata_hash)
  └─ Batches up to 50 CIDs per tx → chronicle-registry SPEL program
      │
      ▼
logos-whistleblower (Qt view plugin)
  └─ QML UI: file picker, publish status, history, anchor config dialog
```

### Components

| Component | Language | Purpose |
|-----------|----------|---------|
| `chronicle_registry_core` | Rust | Shared types (Registry, CidRecord) with Borsh + Serde |
| `methods/guest` | Rust / RISC0 | SPEL guest: `init_registry`, `index_batch` instructions |
| `ffi/` | Rust | C-ABI FFI shim — shells out to `spel` + `lgs wallet` CLIs |
| `batch-anchor/` | Rust | Waku listener → dedup → batch anchor daemon (16/16 tests) |
| `logos-chronicle/` | C++ / Qt | Logos module: upload, broadcast, anchor, publish pipeline |
| `logos-whistleblower/` | C++ / Qt / QML | Logos view plugin: full desktop UI |
| `tools/` | Rust | `mk_program_binary`: builds R0BF-format ProgramBinary |

---

## Metadata hash format

`v1:<sha-256-hex>` over alphabetically-sorted canonical JSON:

```json
{"content_type":"text/plain","description":"…","size_bytes":1024,
 "tags":["a","b"],"timestamp":1751000000,"title":"My Doc"}
```

The hash is stored on-chain in `CidRecord.metadata_hash` and included in every Waku envelope so any node can verify document integrity without fetching from Storage.

---

## Waku envelope format (v=1)

```json
{
  "v": 1,
  "cid": "<Codex CID>",
  "content_type": "text/plain",
  "size_bytes": 1024,
  "timestamp": 1751000000,
  "title": "My Document",
  "description": "Optional summary",
  "tags": ["evidence", "finance"],
  "metadata_hash": "v1:<sha256hex>"
}
```

Topic: `/chronicle/1/document-index/json`

---

## On-chain registry (SPEL program)

Program: `chronicle_registry` — RISC0 guest compiled with `spel` framework.

```
PDA layout (Borsh):
  u32               entries_count
  [entries_count × (
    u32  key_len
    [u8] cid          (UTF-8)
    [u8; 32] metadata_hash
    i64  anchor_timestamp
    [u8; 32] anchored_by
    u8   version
  )]
```

Instructions:
- `init_registry` — creates PDA, sets owner, one-time call
- `index_batch` — appends up to 50 `(cid, metadata_hash, anchor_timestamp)` tuples per tx

---

## Compute unit benchmarks

> Measured on LEZ devnet, `RISC0_DEV_MODE=0`

| Operation | Batch size | CU used |
|-----------|-----------|---------|
| `init_registry` | — | _TBD after deploy_ |
| `index_batch` | 1 CID | _TBD_ |
| `index_batch` | 50 CIDs | _TBD_ |

_(Benchmarks to be filled in after sequencer deploy — program_id and CU counts are extracted by CI in `.github/workflows/ci.yml` anchor job.)_

---

## Smoke tests

```bash
# Storage: upload a file, verify CID + size_bytes, confirm title not source path in logs
nix run .#smoke-storage

# Broadcast: build envelope, send via Waku, verify propagation log lines
nix run .#smoke-broadcast

# Publish: full upload + broadcast pipeline, confirm ledger survives restart
nix run .#smoke-publish

# Anchor: write synthetic CID to on-chain registry, poll PDA until confirmed
ANCHOR_SIGNER=<account_id> nix run .#smoke-anchor
```

All four scripts use `integration-test.toml` at the repo root for isolated topic / program_id — no extra env file needed.

---

## CI

`.github/workflows/ci.yml` runs four jobs:

| Job | Steps |
|-----|-------|
| `fmt` | `cargo fmt --all -- --check` on root + batch-anchor workspaces |
| `build` | `cargo build -p chronicle_registry_core`, batch-anchor, 16 unit tests |
| `publish` | `nix build` chronicle + whistleblower modules |
| `anchor` | `spel program-id --format hex` → `batch-anchor init` → `batch-anchor watch` dry-run |

---

## Running locally

```bash
git clone https://github.com/aegonmyy/logoz
cd logoz

# One-shot bootstrap (build, deploy, mint signer, open registry)
./scripts/setup.sh

# Launch the full app (builds nix modules, starts watcher, opens basecamp)
./scripts/run-app.sh
```

Requires: `lgs` CLI, `nix` with flakes, `docker` (for RISC0 guest build), Rust stable.

---

## Known issues filed

- logos-co/spel-framework#_TBD_ — `spel program-id` requires a pre-built R0BF ProgramBinary, not a raw ELF; format undocumented
- logos-co/spel-framework#_TBD_ — `InvalidProgramBytecode(Malformed ProgramBinary)` when passing guest ELF directly; worked around with `tools/mk_program_binary.rs`

---

## Checklist

- [x] Upload pipeline: file → Logos Storage → CID
- [x] Broadcast pipeline: envelope → Logos Delivery → `/chronicle/1/document-index/json`
- [x] Anchor pipeline: (cid, metadata_hash, timestamp) → LEZ SPEL `index_batch`
- [x] Metadata hash: `v1:<sha256>` over canonical JSON
- [x] Max file size enforced (100 MB)
- [x] Envelope size cap enforced
- [x] Source filename never reaches Storage (title-derived staging)
- [x] Batch dedup by (CID, metadata_hash)
- [x] MAX_BATCH = 50 CIDs per tx
- [x] Waku store catch-up on `batch-anchor watch` start
- [x] Publish ledger persists across restarts
- [x] Anchor ledger persists across restarts
- [x] 16/16 unit tests pass
- [x] `cargo fmt --check` clean
- [x] CI pipeline defined
- [x] Smoke tests for all four pipeline stages
- [x] Desktop UI with history, anchor config dialog, connection status
- [ ] CU benchmarks (pending sequencer deploy)
- [ ] Demo video
