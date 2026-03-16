#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(dirname "$0")"
exec poetry --project "$SCRIPT_DIR" run python "$SCRIPT_DIR/nvmeib_pet_messages.py" "$@"
