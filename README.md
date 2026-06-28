# Whistleblower

A censorship-resistant document upload + indexing pipeline for the Logos stack
— submission for LP-0017.

Anyone can upload a document, have its content-identifier (CID) broadcast
peer-to-peer in real time, and optionally anchor that CID on-chain so it
remains discoverable indefinitely without a trusted host.

> **Why this exists.** Whistleblowers, journalists, and activists need
> publication tools that survive takedowns. Existing options either rely on a
> single trusted host, require the publisher to hold tokens, or provide no
> long-term discoverability guarantee. Whistleblower stitches together three
> Logos primitives — Storage, Delivery, and the LEZ — into a complete
> publish-and-anchor flow, and extracts the core into a reusable module any
> other Basecamp app can drop in.

## Architecture

```
┌─────────────────┐   upload   ┌──────────────┐
│  Basecamp app   │ ──────────▶│ Logos Storage│
│ (logos-         │            │   (Codex)    │
│  whistleblower) │            └──────┬───────┘
└────────┬────────┘                   │  CID
         │ broadcast(CID,metadata)    │
         ▼                            ▼
┌─────────────────┐ ◀───── subscribe ─────────┐
│  Logos Delivery │                            │
│     (Waku)      │                            │
└────────┬────────┘                            │
         │  envelope                           │
         ▼                                     │
┌─────────────────┐    index_batch     ┌──────┴──────────┐
│   batch-anchor  │ ────────────────▶  │ chronicle-      │
│      CLI        │  (50 CIDs/tx)      │ registry (LEZ)  │
└─────────────────┘                    └─────────────────┘
```

## Components

| Path | Purpose |
|---|---|
| **[logos-whistleblower/](logos-whistleblower/)** | Basecamp app GUI (QML + C++) |
| **[logos-chronicle/](logos-chronicle/)** | Reusable document-indexing Qt module: upload → broadcast → anchor |
| **[batch-anchor/](batch-anchor/README.md)** | Permissionless CLI daemon: watches Waku, anchors CIDs in batches of ≤50 |
| **[chronicle_registry_core/](chronicle_registry_core/src/lib.rs)** | Shared on-chain/off-chain types |
| **[methods/guest/](methods/guest/src/bin/chronicle_registry.rs)** | RISC0 ZK guest program (SPEL framework) |
| **[ffi/](ffi/src/lib.rs)** | C ABI cdylib bridge (Rust → C++) |

## Quick Start

### Prerequisites
- `lgs` (logos-scaffold) — install via `cargo install --git https://github.com/logos-co/logos-scaffold logos-scaffold`
- Docker (for RISC0 guest build + nwaku)
- Nix (for Basecamp module build via `flake.nix`)

### 1. Build the guest + generate IDL

```sh
# Build RISC0 guest binary (requires Docker)
make build

# IDL is already committed; regenerate if guest source changes:
make idl
```

### 2. Start localnet + deploy

```sh
lgs setup
lgs localnet start

# Deploy chronicle-registry. program_id is computed dynamically:
make deploy

# Check the deployed program_id:
make inspect
```

### 3. Mint a signer + initialize registry

```sh
lgs wallet -- account new public
lgs wallet topup <ACCOUNT_ID>

# Update batch-anchor/batch-anchor.toml:
#   signer_account_id = "<ACCOUNT_ID>"
#   program_id        = "<output of: make inspect --format hex>"

cd batch-anchor && cargo build
./target/debug/batch-anchor init
```

### 4. Start nwaku + batch-anchor

```sh
./target/debug/batch-anchor node up
./target/debug/batch-anchor watch &
```

### 5. Publish a document (via GUI)

```sh
./demo.sh
```

Or manually via the Basecamp UI: `lgs basecamp launch`.

## Querying the registry

```sh
./target/debug/batch-anchor lookup <CID>
./scripts/list-registry.sh
```

## Building the Basecamp module (Nix)

```sh
make ffi           # builds Rust FFI .so
nix build .#chronicle-install
nix build .#whistleblower-install
lgs basecamp install chronicle
lgs basecamp install whistleblower
lgs basecamp launch
```

## Running CI locally

```sh
./scripts/ci-local.sh
```

## LP-0017 Requirement Map

| # | Requirement | Where |
|---|---|---|
| F1 | Upload to Logos Storage → CID | `logos-chronicle/src/chronicle_plugin.cpp` |
| F2 | Broadcast metadata envelope to Logos Delivery | `logos-chronicle/src/chronicle_plugin.cpp` |
| F3 | UI "Anchor on-chain" action | `logos-whistleblower/src/qml/Main.qml` + `logos-chronicle/src/chronicle_plugin.cpp` |
| F4 | Batch anchor CLI | `batch-anchor/` |
| F5a | On-chain registry (LEZ SPEL program) | `methods/guest/src/bin/chronicle_registry.rs` + IDL at `idl/chronicle-registry.json` |
| F6 | Document-indexing module (extracted, reusable) | `logos-chronicle/` |

## Deployed Registry

Program ID (devnet):
```
7ab2ad5dbcceca1a9d233b675f1c885162672599096cc4f1c604d415a42eef32
```

> **Note**: This ID is from the deterministic build at this commit. The
> CI anchor job extracts it dynamically via `spel program-id` rather than
> reading this file, ensuring the on-chain program always matches the
> built binary.

## License

MIT OR Apache-2.0
