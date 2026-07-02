# Live LEZ v0.2.0 Testnet Evidence — 2026-07-02

The Whistleblower on-chain registry is deployed and exercised **end-to-end on
the hosted LEZ v0.2.0 testnet**, at `RISC0_DEV_MODE=0` (real Groth16 proofs).
This supersedes the earlier "public testnet was wiped, demo runs on localnet"
note: the hosted testnet is live again at the `v0.2.0` tag, and the exact
Program ID and PDA documented in the README are reproduced on-chain.

## Environment

| Field | Value |
| --- | --- |
| Network | hosted LEZ testnet |
| Endpoint | `https://testnet.lez.logos.co/` |
| LEZ ref | `v0.2.0` |
| LEZ commit | `a58fbce2ff48c58b7bb5001b1a27e64b9596ee3a` |
| Wallet | built from `logos-execution-zone` @ `a58fbce2` (v0.2.0 final) |
| `RISC0_DEV_MODE` | `0` |
| Date | 2026-07-02 UTC |
| Block range | ~2381 → 2400 |

Compatibility gate passed before any state-changing transaction:

```
$ wallet check-health
✅All looks good!
```

## Deployed program

| Field | Value |
| --- | --- |
| Program ID (image ID) | `96ad78fec3f1e5013967744268a2a1ac764e0c0cedc1713669e225d310b03e7b` |
| Registry PDA (seed `"registry"`) | `HvCtoPL6RvYmqg8m7qkRqu4e2b1dHt4XsU2dz8eixdwe` |
| Program binary | `methods/guest/target/riscv32im-risc0-zkvm-elf/docker/chronicle_registry.bin` |

The Program ID is the RISC0 image ID derived deterministically from the guest
source at this commit (`spel program-id …` / `make build`), so it matches the
value in `README.md` and `SUBMISSION_PR.md` exactly.

## Signer account + funding (Piñata faucet)

| Field | Value |
| --- | --- |
| Signer | `Public/Pc7pxJm3xjUvon3AL7NrM7qfXZiCkT2LGYXGJyH8cet` |
| `auth-transfer init` tx | `dba386f8019de82d0fd3b710c7f70b62609eccfca626d994fe3470b5ee36eaa8` |
| Piñata `claim` tx | `eb262291fac4f19aca56465427425f793017b28ea233b607ef2011854b5daf4e` |
| Balance after claim | `150` |

## End-to-end anchor flow

1. **Publish** — a document-index envelope was broadcast to the Logos Delivery
   topic `/chronicle/1/document-index/json` and confirmed present on the topic
   (findable via nwaku store).
2. **Anchor** — `batch-anchor watch` picked up the broadcast CID, batched it,
   and submitted `index_batch` to the registry with a **real Groth16 proof**
   (`RISC0_DEV_MODE=0`, ~63 s). Tx confirmed in a block.
3. **Confirm** — `batch-anchor lookup` resolves the CID from on-chain registry
   state; a bogus CID is reported not-registered (exit 1).

| Field | Value |
| --- | --- |
| Anchored CID | `bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi` |
| metadata_hash | `d589da5dc9c2f751409669f9f0df72baab52683f4824ae5f8b760f07b5f64ad5` |
| anchor_timestamp | `1782968046` |
| anchored_by | `05ca7c4a989a05f6a0295993ee176558ffe4ba1173b292949acf96c3ff528759` |
| `index_batch` tx | `02d8781403acc8f851eb87c1c8ffa29183a352a3d8733d3dd87e2eeb051b494a` |
| On-chain confirmation | `✅ Transaction confirmed — included in a block` |

### Lookup (registry confirms the CID)

```
$ batch-anchor lookup bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi
CID:               bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi
metadata_hash:     d589da5dc9c2f751409669f9f0df72baab52683f4824ae5f8b760f07b5f64ad5
anchor_timestamp:  1782968046
anchored_by:       05ca7c4a989a05f6a0295993ee176558ffe4ba1173b292949acf96c3ff528759
version:           1
# exit 0

$ batch-anchor lookup bafybeibogus…
bafybeibogus…: not registered
# exit 1
```

### On-chain transaction (independent verification)

```
$ wallet chain-info transaction --hash 02d8781403acc8f851eb87c1c8ffa29183a352a3d8733d3dd87e2eeb051b494a
Public(PublicTransaction {
    message: Message {
        program_id: "96ad78fec3f1e5013967744268a2a1ac764e0c0cedc1713669e225d310b03e7b",
        account_ids: [
            HvCtoPL6RvYmqg8m7qkRqu4e2b1dHt4XsU2dz8eixdwe,   # registry PDA
            Pc7pxJm3xjUvon3AL7NrM7qfXZiCkT2LGYXGJyH8cet,    # signer
        ],
        …
```

The full watcher log for this run (including the serialized `index_batch`
instruction and proof/submit lines) is preserved alongside this file at
[`testnet-v020-anchor-watch-log-20260702.txt`](testnet-v020-anchor-watch-log-20260702.txt).

## Reproduction

The one-command **localnet** demo (`RISC0_DEV_MODE=0 ./scripts/demo.sh`) remains
the reproducible reference, deriving the same Program ID / PDA. To reproduce
against the hosted testnet:

1. Build a `v0.2.0`-final wallet: `logos-execution-zone` @ `a58fbce2`,
   `cargo build --release -p wallet`; point `sequencer_addr` at
   `https://testnet.lez.logos.co/` and confirm `wallet check-health`.
2. `wallet account new public` → `wallet auth-transfer init` →
   `wallet pinata claim --to <acct>` (faucet-funds the signer).
3. `wallet deploy-program methods/…/chronicle_registry.bin`.
4. Point `batch-anchor` config at the testnet sequencer + funded signer +
   Program ID `96ad78fe…`; run `batch-anchor init`, broadcast a CID, run
   `batch-anchor watch`, then `batch-anchor lookup <cid>`.
