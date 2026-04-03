#!/bin/sh
# Run the standalone QUIC/H3 server example.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

"$PROJECT_ROOT/build/bin/server" 4433 "$1" "$2"
