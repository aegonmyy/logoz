# Chronicle — document-indexing module

`chronicle` is a self-contained Logos Core module that implements the
**upload → broadcast → anchor** pipeline for censorship-resistant document
publishing. It is extracted from the Whistleblower app so that **any** Logos
application can reuse the pipeline without depending on the Whistleblower GUI.

```
file ──▶ upload (Logos Storage / Codex) ──▶ CID
                                             │
          ┌──────────────────────────────────┘
          ▼
   broadcast metadata envelope (Logos Delivery / Waku)
          │   topic: /chronicle/1/document-index/json
          ▼
   anchor (optional, any time)  ──▶ on-chain CidRecord (LEZ SPEL program)
```

The module owns no UI. It exposes a JSON-in/JSON-out method API over the Logos
Core plugin interface; the Whistleblower QML app and the `batch-anchor` CLI are
two independent consumers of it.

## Module metadata

`metadata.json` declares the module to Basecamp / Logos Core:

| Field | Value |
|-------|-------|
| `name` | `chronicle` |
| `type` | `core` |
| `main` | `chronicle_plugin` |
| `dependencies` | `storage_module`, `delivery_module` |

It depends only on the upstream Logos Storage and Delivery modules — not on the
Whistleblower app — which is what makes it reusable.

## Building

The module builds to a Logos Core plugin and a loadable `.lgx` artifact via the
repo flake:

```bash
nix build .#chronicle-install   # plugin + metadata, staged under result/modules
nix build .#chronicle-lgx       # packaged .lgx artifact
```

The on-chain anchor path is a thin C-ABI shim (`libchronicle_registry_ffi.so`,
built via `make ffi`) that shells out to the `spel` / `lgs` CLIs; the prebuilt
`.so` is vendored at `logos-chronicle/vendored/` so the nix build needs no LEZ
toolchain.

## API

All methods are `Q_INVOKABLE`, take a single JSON string (or simple scalars),
and return a JSON string. Successful results carry `"ok": true`; failures carry
`"ok": false` and an `"error"` / `"error_code"`.

### Upload (Logos Storage)

| Method | Purpose |
|--------|---------|
| `uploadFileJson(path, contentType)` | Upload a file; returns `{ok, upload_id}`. Retries transient failures with exponential back-off (1→30 s) and surfaces `RETRIES_EXHAUSTED` after exhaustion. The source filename never reaches Storage — a title-derived staging name is used. |
| `uploadStatusJson(id)` | Poll an upload; returns `{ok, status, cid, size_bytes, ...}` (`status` ∈ queued/uploading/uploaded/retrying/error). |

### Metadata

| Method | Purpose |
|--------|---------|
| `normalizeContentTypeJson(ct)` | Canonicalise a MIME type. |
| `hashMetadataJson(contentType, …)` | Compute `metadata_hash` = `v1:<sha256>` over alphabetically-sorted canonical JSON. |
| `buildMetadataEnvelopeJson(…)` | Build the Delivery envelope (see format below). |

### Broadcast (Logos Delivery)

| Method | Purpose |
|--------|---------|
| `startBroadcasterJson()` | Subscribe/prepare the Delivery topic. |
| `broadcastEnvelopeJson(envelopeJson)` | Publish an envelope; returns `{ok, broadcast_id}`. De-duplicated by CID — re-broadcasting the same CID does not produce a duplicate. |
| `broadcastStatusJson(id)` | Poll a broadcast (`queued`/`sent`/`error`). |
| `setBroadcastTopic(topic)` / `getBroadcastTopic()` | Override / read the Delivery topic. |

### Publish (upload + broadcast in one call)

| Method | Purpose |
|--------|---------|
| `publishFileJson(reqJson)` | One-shot: upload then broadcast. Request keys: `path`, `title`, `description`, `content_type`, `tags` (array), `broadcast` (bool, default `true`). Returns `{ok, publish_id}`. |
| `publishStatusJson(id)` | Poll the combined pipeline; links the `upload_id` and `broadcast_id`. |
| `listPublishedJson()` / `clearPublishedJson()` | The publish ledger (persists across restarts). |

### Anchor (LEZ SPEL registry)

| Method | Purpose |
|--------|---------|
| `anchorCapabilitiesJson()` | Report whether anchoring is configured + which fields are missing. |
| `getAnchorConfigJson()` / `setAnchorConfigJson(cfgJson)` | Read / write the anchor config (program id, signer, wallet home, sequencer). Persisted via `AnchorStore`. |
| `anchorBatchJson(reqJson)` | Anchor one or more `(cid, metadata_hash, anchor_timestamp)` tuples on-chain (≤ 50 per tx). Returns `{ok, anchor_id}`. |
| `anchorStatusJson(id)` | Poll an anchor tx. |
| `lookupAnchorJson(cid)` | Query the registry by CID; returns the stored `CidRecord` or not-found. |
| `listAnchorsJson()` / `clearAnchorsJson()` | The local anchor ledger (persists across restarts). |
| `initRegistryJson()` / `getRegistryJson()` | Open the registry PDA / fetch its current contents. |

### Misc

| Method | Purpose |
|--------|---------|
| `health()` | Liveness + dependency-readiness. |
| `version()` | Module version string. |

## Delivery envelope format (`v = 1`)

Published to `/chronicle/1/document-index/json`:

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

`metadata_hash` lets any subscriber verify document integrity without fetching
the bytes from Storage, and is the value committed on-chain in `CidRecord`.

## Integrating into another Logos app

1. Add `chronicle` to your app's module dependencies (it transitively pulls in
   `storage_module` and `delivery_module`).
2. Stage it into your module set: `nix build .#chronicle-install` and point your
   app at `result/modules` (see `scripts/run-app.sh` for a working example).
3. From your app, call the JSON API above on the `chronicle` module — e.g.
   `publishFileJson` for the upload+broadcast flow, then `anchorBatchJson`
   (or run the standalone `batch-anchor` CLI) for on-chain indexing.
4. Subscribe to `/chronicle/1/document-index/json` to discover documents
   broadcast by any peer, independent of who anchors them.

For on-chain queries from outside the module, the `batch-anchor` CLI exposes
`lookup <cid>` against the same registry; see `../batch-anchor/README.md`.
