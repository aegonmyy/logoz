# batch-anchor

Permissionless batch-anchor CLI for the [chronicle-registry](..) LEZ program.
Subscribes to a Logos Delivery (Waku) content topic, accumulates broadcast
CIDs in memory, and commits them to the on-chain registry in batches.

Lives as a sibling subdirectory of the chronicle-registry SPEL project because
it depends tightly on it: the prebuilt `chronicle_registry_cli` binary, the
program IDL, the signer wallet in `.scaffold/wallet`, and the `lgs` CLI's
requirement that it be invoked from inside a logos-scaffold project root.

This is the standalone tool LP-17 (Whistleblower) requires: any third party can
run it without coordination with the original publisher, and re-running it
after a crash never re-anchors a CID that's already on-chain.

## What it does

Three-hop pipeline:

```
Logos module broadcasts CID                     │ batch-anchor
      │                                         │   subscribes here
      ▼                                         │
   nwaku  ───────────────────────────────────────►  in-memory buffer
   (relay + SQLite store)                       │   (≤ MAX_BATCH=50)
                                                │       │
                                                │       │  flush trigger:
                                                │       │  size OR time
                                                │       ▼
                                                │   chronicle_registry_cli
                                                │      index-batch
                                                │       │
                                                │       ▼
                                                │   chronicle-registry PDA
                                                │   on LEZ devnet
```

Three layers handle persistence:

| Layer            | Lives in                | Survives    | Used for                                  |
|------------------|-------------------------|-------------|-------------------------------------------|
| Waku message log | `./store/store.sqlite3` | node restart | store-protocol catch-up at anchor startup |
| Pending buffer   | RAM                     | nothing     | batching ≤ 50 CIDs per index-batch tx     |
| Registry PDA     | LEZ chain               | everything   | permanent dedup set, seeded at startup    |

On startup the anchor:

1. Reads the registry PDA, decodes it, builds a `HashSet<String>` of
   already-anchored CIDs — the dedup seed.
2. Queries nwaku's `/store/v3/messages` over a configurable lookback window
   (default 24h). Any envelope already in the dedup set is skipped; anything
   else lands in the buffer, queued for the first flush.
3. Subscribes to the live relay topic and starts the poll loop.

Once running, the loop drains the relay queue every `poll_ms` and submits a
batch when either the buffer reaches `max_size` or `flush_after_s` seconds
elapse since the first entry of the current window arrived.

## Dependencies

External runtime requirements:

| Dep                            | Why                                                                            | Where                                                      |
|--------------------------------|--------------------------------------------------------------------------------|------------------------------------------------------------|
| `docker` + `docker compose`    | Run the local nwaku container                                                  | host                                                       |
| `lgs` (logos-scaffold CLI)     | Read raw account bytes for PDA decoding                                        | `$PATH`                                                    |
| chronicle-registry repo        | Provides `spel.toml`, `.scaffold/wallet`, the prebuilt CLI binary              | parent directory (`..`)                                    |
| Built registry CLI binary      | Invoked directly to submit `init_registry` / `index_batch`                     | `../target/debug/chronicle_registry_cli` (run `make build`) |
| spel fork with `Vec<String>` patch | The registry's `index_batch` takes `Vec<String>` CIDs from the CLI         | pinned in `../scaffold.toml`                              |
| LEZ sequencer                  | Where transactions actually execute                                            | reachable at `registry.sequencer_url`                      |

You do **not** need:

- `cargo run` against the registry workspace at submit-time — the prebuilt
  binary is invoked directly.
- A signer of your own — `lgs` reads the existing signer from
  `../.scaffold/wallet`. The anchor inherits whatever identity the registry
  was set up with.

batch-anchor is **excluded** from the chronicle-registry Cargo workspace
(`exclude = ["batch-anchor"]` in the parent `Cargo.toml`) so its tokio /
reqwest dependencies stay out of the risc0 guest build.

## Configuration

All values live in `batch-anchor.toml`. Every field is required. Env vars
override file values (handy in CI / containers):

```toml
[delivery]
rest_url             = "http://127.0.0.1:8645"            # nwaku REST API
content_topic        = "/chronicle/1/document-index/json" # Waku topic to subscribe to
poll_ms              = 1000                                # relay-drain interval
store_lookback_hours = 24                                  # store-protocol catch-up window (0 disables)

[registry]
spel_toml         = "../spel.toml"          # finds registry_dir, registry_cli_bin, IDL
sequencer_url     = "http://127.0.0.1:3040"
wallet_home       = "../.scaffold/wallet"
signer_account_id = "DkyMG8uaEJqv1A8at8b8cdktNeaaMhaWY57VETgEVaMX"
program_id        = "6ac5aa11b87bcd1961c7b5294b8d01e8746b2103e3beb665202a0299d2cf0252"  # 64-char hex, pin to a deployed registry

[batch]
max_size      = 50    # MAX_BATCH from chronicle_registry_core; on-chain ceiling
flush_after_s = 30    # time-based flush trigger

[node]
compose_file  = "docker-compose.yml"
```

Env var pattern: `BATCH_ANCHOR_<SECTION>__<KEY>`, double-underscore between
section and key, uppercase. Example:

```bash
BATCH_ANCHOR_DELIVERY__REST_URL=http://nwaku.internal:8645 \
BATCH_ANCHOR_BATCH__MAX_SIZE=25 \
  batch-anchor watch
```

## Quick start

From scratch:

```bash
# 1. Make sure chronicle-registry is built and deployed (one-time).
( cd .. && make build deploy setup )

# 2. Point batch-anchor.toml at the registry's signer and program ID.
SIGNER=$(grep SIGNER_ID ../.chronicle_registry-state | cut -d= -f2)
sed -i "s/^signer_account_id = .*/signer_account_id = \"$SIGNER\"/" batch-anchor.toml
# Capture the deployed program ID from a CLI dry-run (any instruction prints it).
PROGRAM_ID=$(cd .. && ./target/debug/chronicle_registry_cli init-registry \
              --anchorer "$SIGNER" --dry-run=text 2>&1 \
              | awk '/^Program ID:/ {print $3}')
sed -i "s/^program_id        = .*/program_id        = \"$PROGRAM_ID\"/" batch-anchor.toml

# 3. Start the local nwaku node.
cargo run -- node up

# 4. Open the registry (idempotent — fine to run twice).
cargo run -- init

# 5. Start the anchor.  Broadcast from anywhere on the cluster — it'll catch up.
cargo run -- watch
```

You should see:

```
INFO Seeding dedup set from on-chain registry ...
INFO 0 CIDs already anchored on-chain
INFO nwaku ready
INFO Catching up via store-protocol (last 24h) ...
INFO Catch-up: 0 historical message(s) seen, 0 new to anchor
INFO Subscribed to /chronicle/1/document-index/json
INFO Polling every 1000ms — send broadcasts now ...
```

## Commands

### `watch`

The daemon. Runs the full pipeline:

```bash
batch-anchor watch
```

Startup sequence (`cmd::watch::run`):

1. **Build RegistryClient** — canonicalises paths, verifies the prebuilt
   `chronicle_registry_cli` exists, fails fast otherwise.
2. **Seed dedup set** — runs `chronicle_registry_cli --dry-run=text init-registry`
   to derive the PDA, then `lgs wallet -- account get --raw` to fetch its bytes,
   then Borsh-decodes a `Registry { entries: HashMap<String, CidRecord> }`. The
   keys become a `HashSet<String>` passed to the buffer.
3. **Wait for nwaku** — health-checks `GET /health` up to 30s.
4. **Catch-up** (skipped if `store_lookback_hours = 0`) — paginated
   `GET /store/v3/messages?contentTopics=...&startTime=...&includeData=true`,
   decoding each payload via `Envelope::from_payload`. Already-known CIDs are
   dropped by the buffer's `push` dedup; the rest queue up.
5. **Subscribe** — `POST /relay/v1/auto/subscriptions [content_topic]`.
6. **Loop**:
   - sleep `poll_ms`
   - `GET /relay/v1/auto/messages/<topic>` → drains nwaku's per-subscription queue
   - parse each payload as `Envelope` (validates `v == 1`, `"v1:"` hash prefix,
     non-empty CID, non-zero timestamp)
   - `buffer.push(env)` — silently skips known or in-buffer duplicates
   - if `buffer.should_flush()`:
     - drain the buffer
     - call `RegistryClient::index_batch(...)` on a `spawn_blocking` task
     - on success: `mark_flushed` (move CIDs into the dedup set)
     - on failure: `return_failed` (put the batch back at the front of the buffer)

Submission spawns `chronicle_registry_cli index-batch --cids c1,c2,... --metadata-hashes h1,h2,... --anchor-timestamps t1,t2,... --anchorer <signer>`.

### `init`

Idempotent registry opener:

```bash
batch-anchor init
```

Pre-checks PDA existence (via `derive_pda` + `fetch_account_bytes`) and exits
with `Registry already initialised` if it's already there. Only submits an
actual `init_registry` transaction on first run. Avoids the CLI's "waiting for
confirmation" hang when the sequencer rejects a duplicate init.

### `lookup <cid>`

Single-CID query:

```bash
batch-anchor lookup bafychronicle-smoke-1778515025
```

Decodes the registry PDA and looks the CID up in the `HashMap`. Prints the full
`CidRecord` if present:

```
CID:               bafychronicle-smoke-1778515025
metadata_hash:     949dc7f544d1e1412d7b3c83b1a54cdec8c8c8afb7b7645640c1d443721c896d
anchor_timestamp:  1778515025
anchored_by:       bd8feeaacd0e027d58d09ca157e35470d2de4bebc950739a682b2f565f11e2da
version:           1
```

Exit codes:

| Code | Meaning                                                |
|------|--------------------------------------------------------|
| 0    | CID found in registry                                  |
| 1    | CID not registered                                     |
| 2    | Registry PDA does not exist (need to run `init` first) |

### `node {up,down,status,logs}`

Thin wrapper over `docker compose -f <compose_file>`:

```bash
batch-anchor node up       # idempotent — no-op if already running
batch-anchor node status   # `docker compose ps`
batch-anchor node logs     # `docker compose logs -f`
batch-anchor node down     # stop + remove container (SQLite volume persists)
```

The compose file (`docker-compose.yml`) configures nwaku for the **Logos Dev
Network**: `cluster-id=2`, 8-shard autosharding, no RLN, discovery via the
`logos.dev` fleet entry nodes. These settings mirror what `delivery_module`
uses internally (see `logos-messaging/logos-delivery/waku/factory/networks_config.nim`,
`LogosDevConf`), which is why broadcasts from any Logos module on that cluster
land in our node's relay queue.

`node down` stops the container but the `./store/` volume keeps the SQLite —
so on `node up` we resume with full history. Wipe with `rm -rf store/` if
needed.

## Envelope schema

Subscribed payloads must conform to:

```json
{
  "cid": "bafy...",
  "metadata_hash": "v1:<64-char-lowercase-hex>",
  "timestamp": 1715000000,
  "v": 1,
  ...
}
```

`Envelope::from_payload` validates `v == 1`, decodes the 32-byte hash from the
`"v1:"` prefix, and rejects empty CIDs or zero timestamps. Additional fields
(title, description, content_type, tags, size_bytes) are ignored — they're for
human-facing discovery, not for anchoring.

## Reliability story (LP-17 R3)

> "The batch anchor tool resumes from the last successfully anchored batch
> after a network interruption, without re-processing already-registered CIDs."

Three pieces make that work:

1. **No local state file.** The dedup set is derived from on-chain state on
   every startup, so it can't drift.
2. **nwaku's SQLite store** keeps every Waku message the node has seen,
   independent of our anchor's uptime. If the anchor was down for an hour
   while broadcasts kept arriving, nwaku still has them.
3. **Store-protocol catch-up.** On startup, after seeding from chain, the
   anchor queries `/store/v3/messages` over a configurable lookback window
   and feeds anything not already on-chain into the buffer. The next flush
   picks them up.

The window is bounded by `store_lookback_hours`. If you've been down longer
than the window, increase it for one run, or accept that anything broadcast
beyond the window may have aged out of other peers' stores too and rely on
re-broadcast.

## Project layout

```
chronicle-registry/
├── ...                    # SPEL program (chronicle_registry_core, methods, examples)
└── batch-anchor/          # this directory
    ├── Cargo.toml
    ├── batch-anchor.toml          # default config (overridable via -c / env)
    ├── docker-compose.yml         # nwaku v0.38.0, Logos Dev cluster 2
    ├── store/                     # nwaku SQLite, mounted into the container
    └── src/
        ├── main.rs                # clap dispatch (watch / init / lookup / node)
        ├── config.rs              # TOML + env overrides
        ├── batch/mod.rs           # in-memory buffer, size/time flush triggers
        ├── delivery/
        │   ├── client.rs          # nwaku REST: subscribe, drain, query_store
        │   └── envelope.rs        # payload decode + validation
        ├── registry/
        │   ├── state.rs           # RegistryClient: derive PDA, fetch, init, index_batch
        │   └── types.rs           # local Borsh mirror of chronicle_registry_core
        └── cmd/
            ├── watch.rs           # the daemon
            ├── init.rs
            ├── lookup.rs
            └── node.rs            # docker-compose wrapper
```

## Tests

```bash
cargo test
```

Pure-Rust unit tests cover the envelope parser (5 cases including version and
hash-format rejection), the batch buffer (6 cases: dedup, flush triggers, drain
/ mark / return semantics), the Borsh roundtrip on `Registry`, and the
PDA-line parser. Live end-to-end coverage is exercised by running `watch`
against a real nwaku + registry and verifying CIDs land in the PDA.
