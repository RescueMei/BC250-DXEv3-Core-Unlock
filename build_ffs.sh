#!/usr/bin/env bash
set -euo pipefail

# Thin wrapper around the shared build helper in the DXEBUILD submodule.  The
# real build logic lives in DXEBUILD/build_ffs.sh; this repository only
# supplies the per-project build.conf and points the shared script at the
# repository root.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SHARED="$SCRIPT_DIR/DXEBUILD/build_ffs.sh"

if [[ ! -f "$SHARED" ]]; then
  echo "error: shared build script not found: $SHARED" >&2
  echo "Initialize the DXEBUILD submodule first:" >&2
  echo "  git submodule update --init --recursive" >&2
  exit 1
fi

export REPO_DIR="$SCRIPT_DIR"
exec "$SHARED" "$@"
