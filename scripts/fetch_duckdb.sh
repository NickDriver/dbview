#!/usr/bin/env bash
# Vendor the prebuilt DuckDB C library (duckdb.h + libduckdb dylib/so) for the engine.
# Dev uses the shared lib (fast iteration); a static build is a release concern (SPEC §9).
# Pinned to a known version for reproducibility. Run once; re-run is a no-op if present.
set -euo pipefail

DUCKDB_VER="${DUCKDB_VER:-v1.5.4}"
DEST="src/vendor/duckdb"

cd "$(dirname "$0")/.."

if [ -f "$DEST/duckdb.h" ]; then
  echo "duckdb already vendored at $DEST ($(cat "$DEST/VERSION" 2>/dev/null || echo unknown))"
  exit 0
fi

case "$(uname -s)-$(uname -m)" in
  Darwin-*)        ASSET="libduckdb-osx-universal.zip" ;;
  Linux-x86_64)    ASSET="libduckdb-linux-amd64.zip" ;;
  Linux-aarch64)   ASSET="libduckdb-linux-arm64.zip" ;;
  *) echo "unsupported platform: $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

URL="https://github.com/duckdb/duckdb/releases/download/${DUCKDB_VER}/${ASSET}"
echo "Fetching $URL ..."
mkdir -p "$DEST"
tmp="$(mktemp -d)"
curl -fsSL "$URL" -o "$tmp/duckdb.zip"
unzip -oq "$tmp/duckdb.zip" -d "$DEST"
rm -rf "$tmp"
echo "$DUCKDB_VER" > "$DEST/VERSION"

echo "Vendored DuckDB $DUCKDB_VER:"
ls -1 "$DEST"
