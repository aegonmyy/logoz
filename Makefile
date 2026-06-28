GUEST_SRC := methods/guest/src/bin/chronicle_registry.rs
GUEST_ELF := methods/guest/target/riscv32im-risc0-zkvm-elf/docker/chronicle_registry
GUEST_BIN := methods/guest/target/riscv32im-risc0-zkvm-elf/docker/chronicle_registry.bin
IDL       := idl/chronicle-registry.json
FFI_SO    := logos-chronicle/vendored/libchronicle_registry_ffi.so

.PHONY: build ffi idl deploy inspect fmt clippy test clean all

all: build ffi idl

# Build the RISC0 guest binary using cargo risczero (deterministic Docker build).
# Outputs: $(GUEST_ELF) (raw ELF)
# Then wraps it into risc0 ProgramBinary format: $(GUEST_BIN)
build:
	DOCKER_BUILDKIT=0 docker build \
		-t chronicle-registry-guest \
		-f /tmp/guest.Dockerfile \
		/workspaces/workspace/desk/logos/whistleblower
	docker save chronicle-registry-guest -o /tmp/guest_image.tar
	@mkdir -p $(dir $(GUEST_ELF))
	@tar -xf /tmp/guest_image.tar -C /tmp --wildcards "*.tar" 2>/dev/null; \
	  for blob in /tmp/*.tar; do \
	    if tar -tf "$$blob" 2>/dev/null | grep -q chronicle_registry; then \
	      tar -xf "$$blob" chronicle_registry -O > $(GUEST_ELF); \
	      break; \
	    fi; \
	  done
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
