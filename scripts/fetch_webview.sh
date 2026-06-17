#!/usr/bin/env bash
# Vendor the webview/webview library (system WebView wrapper) for the `app` build target.
# Pin a tag for reproducibility. Run once; re-run is a no-op if already present.
set -euo pipefail

WV_REF="${WV_REF:-0.12.0}"   # override before release with a pinned commit/tag
DEST="src/vendor/webview"

cd "$(dirname "$0")/.."

if [ -d "$DEST/.git" ]; then
  echo "webview already vendored at $DEST (ref: $(git -C "$DEST" rev-parse --short HEAD))"
  exit 0
fi

echo "Cloning webview ($WV_REF) into $DEST ..."
rm -rf "$DEST"
git clone --depth 1 --branch "$WV_REF" https://github.com/webview/webview.git "$DEST" 2>/dev/null \
  || git clone --depth 1 https://github.com/webview/webview.git "$DEST"

echo "Done. webview.cc: $(test -f "$DEST/core/src/webview.cc" && echo present || echo MISSING)"
echo "Now build the app target."
