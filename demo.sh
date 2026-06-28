#!/usr/bin/env bash
# Thin wrapper for the LP-17 evaluator: keeps `demo.sh` discoverable at
# the repo root while delegating to the real launcher under scripts/.
# Everything (sequencer, modules, basecamp, anchor) lives in
# scripts/run-app.sh; see its header for the full step list.
exec "$(dirname "${BASH_SOURCE[0]}")/scripts/run-app.sh" "$@"
