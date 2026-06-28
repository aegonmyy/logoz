#!/usr/bin/env bash
# Run the same checks as CI locally. Keep in sync with .github/workflows/ci.yml.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO"

echo "--- fmt: chronicle-registry workspace"
cargo fmt --all -- --check

echo "--- fmt: batch-anchor"
(cd batch-anchor && cargo fmt --all -- --check)

echo "--- build: chronicle_registry_core"
cargo build -p chronicle_registry_core

echo "--- build: batch-anchor"
(cd batch-anchor && cargo build --bin batch-anchor)

echo "--- test: batch-anchor"
(cd batch-anchor && cargo test)

echo
echo "All checks passed."
