#!/bin/sh
# Run the QUIC server test example.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

"$PROJECT_ROOT/build/bin/quic_server_test" 127.0.0.1 4433 "$1" "$2"
