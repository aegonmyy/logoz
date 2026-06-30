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

> Measured against the public LEZ testnet (`https://testnet.lez.logos.co`, LEZ pin
> `27360cb7`), `RISC0_DEV_MODE=0` (full Groth16 proofs).

| Operation | Batch size | CU (R0VM cycles) | Notes |
|-----------|-----------|------------------|-------|
| `init_registry` | — | 414 | one-time; registry already open, value from the original init run |
| `index_batch` | 1 CID | 6537 | measured 2026-06-30, tx `f14e39c9…` (see Testnet evidence) |
| `index_batch` | 50 CIDs | not yet benchmarked | — |

_CU is reported by the R0VM as `CU index_batch n=<k> cycles=<n>` at proof time._

---

## Testnet evidence (live, RISC0_DEV_MODE=0)

Anchored on the public LEZ testnet `https://testnet.lez.logos.co` with full
Groth16 proofs (no dev-mode). All artifacts below are reproducible from the
binary in this repo (`make build` regenerates the exact program ID).

| Item | Value |
|------|-------|
| Program ID | `96ad78fec3f1e5013967744268a2a1ac764e0c0cedc1713669e225d310b03e7b` |
| Registry PDA | `HvCtoPL6RvYmqg8m7qkRqu4e2b1dHt4XsU2dz8eixdwe` (seed `"registry"`) |
| `index_batch` tx (2026-06-30) | `f14e39c97fe90c5d50a802d1b750e020bd175b86758bc213496ab29779e9d6c6` |
| Anchored by | `a6a60a47…e89d` — an **anonymous** `Private/` account (privacy-preserving tx) |
| Registry contents | 2 CIDs, both retrievable via `batch-anchor lookup <cid>` |

The `index_batch` transaction is **privacy-preserving**: it routes through the
LEZ proving path because the anchorer is a `Private/` account, so the on-chain
record's `anchored_by` is an anonymous key — the whistleblower's identity is not
revealed while the document index stays publicly verifiable.

Reproduce a lookup:

```bash
LEE_WALLET_HOME_DIR=.scaffold/wallet-tn \
  batch-anchor/target/debug/batch-anchor -c <config> lookup \
  bafyregen20260630whistleblower01
# → CID / metadata_hash / anchor_timestamp / anchored_by / version, exit 0
```

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

### Build cache (zerokit / RLN)

The GUI modules transitively build the `zerokit` (RLN) crate, whose vendored-deps
fixed-output derivation is fetched by nixpkgs' `fetch-cargo-vendor-util`. That
helper sends no `User-Agent`, so crates.io 403s it (a stock-nixpkgs bug in the
pinned rev) and a cold `nix build` fails on a clean machine. Because the FOD is
content-addressed and served by no public cache, we host the prebuilt copy on a
public Cachix cache and declare it in `flake.nix` `nixConfig`:

```
extra-substituters        = https://logoz.cachix.org
extra-trusted-public-keys = logoz.cachix.org-1:Jtd4Lh0/abPKd4ojFXPVngEK2nDUr3FIaSRIbGF8kJQ=
```

Run nix with `--accept-flake-config` (or add the two lines to `nix.conf`) and the
FOD is substituted automatically — no crates.io fetch, no auth required (public
read). This is a workaround for the upstream nixpkgs bug, not a project quirk.

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
- [x] Live testnet anchor (privacy-preserving) — tx `f14e39c9…`, reproducible lookup
- [x] CU benchmarks: `init_registry`=414, `index_batch` n=1=6537 (n=50 not yet benchmarked)
- [ ] Demo video
