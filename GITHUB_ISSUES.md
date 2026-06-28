# GitHub Issues to File

File these against the Logos tech repos before submitting the prize PR.
Replace _TBD_ issue numbers in SUBMISSION_PR.md once filed.

---

## Issue 1 — spel-framework (or logos-lez-sequencer)

**Title**: `spel program-id` requires R0BF ProgramBinary format, not raw ELF — undocumented

**Body**:

### Summary

`spel program-id <path>` fails with `InvalidProgramBytecode(Malformed ProgramBinary)` when
given the raw RISC-V ELF produced by `cargo build` for a `#[lez_program]` guest.

The command expects a **R0BF ProgramBinary** — a custom wrapper format defined in `risc0-binfmt`:

```
b"R0BF"           (4-byte magic)
u32 format_version  (= 1)
u32 header_len
[u8 * header_len]  (postcard-encoded SystemState header)
u32 user_elf_len
[u8 * user_elf_len] (the actual guest ELF)
[u8 ...]           (V1COMPAT kernel ELF, appended)
```

This is **not documented** anywhere in the spel-framework README, `spel help`, or the LP-0017
prize spec. The error message gives no hint that the format is wrong.

### Steps to reproduce

```bash
# Build the guest crate
cargo build --target riscv32im-risc0-zkvm-elf -p my_guest

# Pass the raw ELF to spel program-id
spel program-id target/riscv32im-risc0-zkvm-elf/debug/my_guest
# → InvalidProgramBytecode(Malformed ProgramBinary)
```

### Workaround

Build a proper R0BF binary using `risc0-binfmt` + `risc0-zkos-v1compat`:

```rust
use risc0_binfmt::{MemoryImage, Program};
use risc0_zkos_v1compat::KERNEL_ELF;

let elf_bytes = std::fs::read(&elf_path)?;
let program   = Program::load_elf(&elf_bytes, risc0_zkvm::GUEST_MAX_MEM as u32)?;
let image     = MemoryImage::new(&program, risc0_zkvm::PAGE_SIZE as u32)?;
// then serialize via risc0_binfmt::SystemState and postcard into the R0BF layout
```

### Suggested fix

- Document the R0BF format in the spel-framework README under "Deploying your program"
- OR make `spel program-id` accept raw ELF and perform the wrapping internally
- OR emit a human-readable error: `"input is a raw ELF; wrap it first with spel build"`

---

## Issue 2 — spel-framework (or logos-lez-sequencer)

**Title**: `spel deploy` should derive and print program_id without requiring a separate `spel program-id` call

**Body**:

### Summary

The deployment flow requires two separate commands to get the program_id:

```bash
spel deploy my_program.bin          # deploys, prints tx hash but NOT program_id
spel program-id my_program.bin      # separate call to extract the id
```

This is fragile in CI: if the binary changes between the two calls the IDs diverge.
The `deploy` command has already validated the binary — it should print the program_id
as part of its output.

### Suggested fix

Have `spel deploy` output a structured result including both `tx_hash` and `program_id`:

```
Deployed program
  tx_hash:    0xabc...
  program_id: 7ab2ad5d...
```

or as `--format json`:

```json
{"tx_hash": "0xabc...", "program_id": "7ab2ad5d..."}
```

This would let CI scripts do a single `spel deploy --format json | jq -r .program_id`
instead of a two-step build → program-id pipeline.

### Current CI workaround (LP-0017)

```yaml
- name: Extract program_id
  run: |
    HEX=$(spel program-id \
      methods/guest/target/riscv32im-risc0-zkvm-elf/docker/chronicle_registry.bin \
      --format hex)
    echo "PROGRAM_ID=$HEX" >> $GITHUB_ENV
```
