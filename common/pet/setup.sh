#!/usr/bin/env bash
set -euxo pipefail
SCRIPT_DIR="$(dirname "$0")"
poetry --directory "$SCRIPT_DIR" lock
poetry --directory "$SCRIPT_DIR" install --no-root
#TODO somehow we need to install kaitaistruct compiler - https://kaitai.io/#download
#The correct way is to have a local artifacts server and add packages there