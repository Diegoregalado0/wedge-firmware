#!/usr/bin/env bash
# Build the firmware and publish it for the device to pick up.
#
# The version is whatever git describe says, so a release is a tagged commit
# and nothing has to be typed twice. A dirty tree is refused: a device
# reporting a version that cannot be checked out is worse than no version.

set -euo pipefail
cd "$(dirname "$0")/.."

BASE="${WEDGE_BACKEND:-https://wedge-three.vercel.app}"
: "${WEDGE_PASSWORD:?set WEDGE_PASSWORD to the sender password}"

if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "refusing to publish from a dirty tree; commit first" >&2
    exit 1
fi

VERSION="$(git describe --tags)"
echo "building $VERSION"

source "${IDF_PATH:-$HOME/esp/esp-idf-v5.5.4}/export.sh" >/dev/null
( cd platform/esp32s3 && idf.py build >/dev/null )
BIN=platform/esp32s3/build/wedge.bin

# What the image says about itself, which is what the device compares against.
EMBEDDED="$(python3 -c "d=open('$BIN','rb').read(); print(d[48:80].split(b'\x00')[0].decode())")"
if [ "$EMBEDDED" != "$VERSION" ]; then
    echo "the image says '$EMBEDDED' but git says '$VERSION'; not publishing" >&2
    exit 1
fi

COOKIE="$(mktemp)"
trap 'rm -f "$COOKIE"' EXIT
curl -fsS -c "$COOKIE" -X POST "$BASE/api/auth" \
    -H 'Content-Type: application/json' \
    -d "{\"password\":\"$WEDGE_PASSWORD\"}" >/dev/null

curl -fsS -b "$COOKIE" -X POST "$BASE/api/firmware?version=$VERSION" \
    --data-binary "@$BIN" -H 'Content-Type: application/octet-stream' \
    | python3 -m json.tool

echo
echo "published. devices pick it up within a day, install it to the spare slot,"
echo "and keep it only once the new build has reached the backend."
