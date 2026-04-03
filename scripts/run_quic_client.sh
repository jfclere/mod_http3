#!/bin/sh
# Run the QUIC client test example.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

"$PROJECT_ROOT/build/bin/quic_client_test" localhost 4433
