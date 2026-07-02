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

### 5. Run the end-to-end demo

```sh
# Full anchor pipeline against a real local sequencer at RISC0_DEV_MODE=0:
# deploy → fund → init registry → privacy-preserving index_batch (real proof)
# → query by CID. Idempotent; DEMO_RESET=1 wipes the chain and starts clean.
./scripts/demo.sh
```

To publish a document through the GUI instead: `lgs basecamp launch`.

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

## On-chain approach: LEZ SPEL program (not zone SDK)

The registry is implemented as a RISC0 ZK guest program deployed via the SPEL
framework (`#[lez_program]` / `#[instruction]` macros), not via the zone SDK.

**Why LEZ over the zone SDK:**

The zone SDK approach requires a single designated actor to perform consensus
inscription — that actor becomes a centralised trust assumption. Anyone who
controls it can censor or delay anchoring. For a whistleblower tool, that is
exactly the threat model we are trying to eliminate.

A LEZ SPEL program runs inside the sequencer's ZK circuit: every `index_batch`
call produces a RISC0 proof that any peer can verify. No single actor controls
admission. The `init_registry` instruction is permissionless — the PDA is open
to any signer — so the registry itself cannot be seized or blocked without
stopping the entire LEZ network.

The tradeoff is tooling maturity: the zone SDK has a simpler deployment story
today and decentralised sequencers for zones are not yet shipped. We accept
that tradeoff because the security model of the LEZ approach is materially
stronger for the censorship-resistance use case.

## LP-0017 Requirement Map

| # | Requirement | Where |
|---|---|---|
| F1 | Upload to Logos Storage → CID | `logos-chronicle/src/chronicle_plugin.cpp` |
| F2 | Broadcast metadata envelope to Logos Delivery | `logos-chronicle/src/chronicle_plugin.cpp` |
| F3 | UI "Anchor on-chain" action | `logos-whistleblower/src/qml/Main.qml` + `logos-chronicle/src/chronicle_plugin.cpp` |
| F4 | Batch anchor CLI | `batch-anchor/` |
| F5a | On-chain registry (LEZ SPEL program) | `methods/guest/src/bin/chronicle_registry.rs` + IDL at `idl/chronicle-registry.idl.json` |
| F6 | Document-indexing module (extracted, reusable) | `logos-chronicle/` |

## Deployed Registry

Program ID (deterministic, rc5 build):
```
96ad78fec3f1e5013967744268a2a1ac764e0c0cedc1713669e225d310b03e7b
```
Registry PDA (seed `"registry"`): `HvCtoPL6RvYmqg8m7qkRqu4e2b1dHt4XsU2dz8eixdwe`

> **Note**: This ID is reproduced exactly by `make build` at this commit and by
> `./scripts/demo.sh`. The CI anchor job and the demo script both derive it
> dynamically from the built binary rather than trusting this file, so the
> on-chain program always matches the source.

**Live on the hosted LEZ v0.2.0 testnet.** The registry is deployed and
exercised end-to-end on `https://testnet.lez.logos.co/` (LEZ `v0.2.0`, commit
`a58fbce2`) at `RISC0_DEV_MODE=0` — the Program ID and PDA above are reproduced
on-chain, a CID was anchored via a real Groth16 proof
(tx `02d8781403acc8f851eb87c1c8ffa29183a352a3d8733d3dd87e2eeb051b494a`), and
`lookup` confirms it. Full details, tx hashes, and reproduction steps:
[`docs/testnet-v020-live-evidence-20260702.md`](docs/testnet-v020-live-evidence-20260702.md).
The one-command `./scripts/demo.sh` runs the same flow against a local sequencer
for reproducibility.

## License

MIT OR Apache-2.0
