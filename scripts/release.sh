#!/usr/bin/env bash
set -euo pipefail

# scripts/release.sh
# Usage: ./scripts/release.sh
# Builds the project and generates signed release artifacts in build-release/dist/

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
  echo "error: run this script from a Git worktree" >&2
  exit 1
}
cd "$repo_root"

BUILD_DIR="build-release"
DIST_DIR="${BUILD_DIR}/dist"

echo "==> Configuring and building release artifacts..."
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target release -- -j"$(nproc)"

if [[ ! -d "$DIST_DIR" ]]; then
  echo "error: build failed or dist directory not found" >&2
  exit 1
fi

echo "==> Generating checksums and signatures..."
cd "$DIST_DIR"

# Ensure clean slate for signatures
rm -f *.asc SHA256SUMS

GPG_OPTS=("--batch" "--yes" "--detach-sign" "--armor")
if [[ -n "${GPG_KEY:-}" ]]; then
  GPG_OPTS+=("--local-user" "$GPG_KEY")
fi

# Hash and sign each artifact
shopt -s nullglob
for artifact in mod_http3-*; do
  if [[ "$artifact" == *.sha256 || "$artifact" == *.asc ]]; then
    continue
  fi
  
  echo " -> Processing $artifact"
  sha256sum "$artifact" > "${artifact}.sha256"
  gpg "${GPG_OPTS[@]}" --output "${artifact}.asc" "$artifact"
done

# Create and sign the aggregate checksum manifest
echo " -> Generating aggregate SHA256SUMS"
sha256sum *.sha256 > SHA256SUMS
gpg "${GPG_OPTS[@]}" --output SHA256SUMS.asc SHA256SUMS

echo "==> Done. Release artifacts are available in ${DIST_DIR}/"
ls -lh
