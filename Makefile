GUEST_SRC := methods/guest/src/bin/chronicle_registry.rs
GUEST_ELF := methods/guest/target/riscv32im-risc0-zkvm-elf/docker/chronicle_registry
GUEST_BIN := methods/guest/target/riscv32im-risc0-zkvm-elf/docker/chronicle_registry.bin
IDL       := idl/chronicle-registry.json
FFI_SO    := logos-chronicle/vendored/libchronicle_registry_ffi.so

.PHONY: build ffi idl deploy inspect fmt clippy test clean all

all: build ffi idl

# Build the RISC0 guest binary deterministically via `cargo risczero build`
# (runs the risc0 docker toolchain internally — no host-specific paths).
# Outputs the raw ELF at $(GUEST_ELF); mk-program-binary then wraps it into
# the risc0 ProgramBinary format that spel/lgs consume: $(GUEST_BIN).
build:
	cargo risczero build --manifest-path methods/guest/Cargo.toml
	cargo build --release --manifest-path tools/Cargo.toml --bin mk-program-binary
	tools/target/release/mk-program-binary $(GUEST_ELF) $(GUEST_BIN)
	@echo "Built: $(GUEST_BIN)"
	@spel program-id $(GUEST_BIN) --format hex

# Build the Rust FFI cdylib used by logos-chronicle.
ffi:
	cd ffi && cargo build --release
	mkdir -p logos-chronicle/vendored
	cp ffi/target/release/libchronicle_registry_ffi.so $(FFI_SO)
	@echo "FFI built: $(FFI_SO)"

idl:
	mkdir -p idl
	spel generate-idl $(GUEST_SRC) > $(IDL)

deploy:
	@echo "Extracting program_id from binary..."
	@PROGRAM_ID=$$(spel program-id $(GUEST_BIN) --format hex); \
	  echo "Deploying with program_id=$$PROGRAM_ID"; \
	  lgs deploy

inspect:
	spel program-id $(GUEST_BIN)

fmt:
	cargo fmt --all
	cd batch-anchor && cargo fmt --all
	cd ffi && cargo fmt --all

clippy:
	cargo clippy --all-targets -- -D warnings
	cd batch-anchor && cargo clippy --all-targets -- -D warnings
	cd ffi && cargo clippy -- -D warnings

test:
	RISC0_DEV_MODE=1 cargo test --workspace
	cd batch-anchor && cargo test

clean:
	cargo clean
	cd batch-anchor && cargo clean
	cd ffi && cargo clean
	rm -f $(IDL) $(GUEST_BIN) $(FFI_SO)
