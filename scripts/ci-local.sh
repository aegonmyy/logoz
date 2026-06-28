#!/usr/bin/env bash
#
# Run the same checks as .github/workflows/ci.yml against the local
# checkout, so you can catch failures before pushing. Mirrors the two CI
# jobs (build + fmt) — keep this script and ci.yml in sync.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

echo "▸ fmt (chronicle-registry workspace)"
cargo fmt --all -- --check

echo "▸ fmt (batch-anchor)"
(cd batch-anchor && cargo fmt --all -- --check)

echo "▸ cargo build -p chronicle_registry_core"
cargo build -p chronicle_registry_core

echo "▸ cargo build --bin batch-anchor"
(cd batch-anchor && cargo build --bin batch-anchor)

echo "▸ cargo test (batch-anchor)"
(cd batch-anchor && cargo test)

echo
echo "✅ All CI checks passed locally."
