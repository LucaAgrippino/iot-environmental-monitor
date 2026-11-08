#!/usr/bin/env bash
# Pack what the Raspberry Pi needs into one file: the backend source, the built web app, the service files and the
# installer. Nothing is built on the Pi except the Python virtual environment.
#   cloud/deploy/build-bundle.sh        -> cloud/deploy/out/iotcloud-bundle.tar.gz
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cloud="$here/.."
[ -d "$cloud/web/dist" ] || "$cloud/web/build.sh"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/iotcloud-bundle/backend"
cp -r "$cloud/backend/iotcloud" "$cloud/backend/pyproject.toml" "$stage/iotcloud-bundle/backend/"
cp -r "$cloud/web/dist" "$stage/iotcloud-bundle/web"
cp "$here/install.sh" "$here/iotcloud.service" "$here/iotcloud.env.example" "$here/Caddyfile" "$stage/iotcloud-bundle/"
[ -f "$here/requirements.lock" ] && cp "$here/requirements.lock" "$stage/iotcloud-bundle/"
find "$stage" -name __pycache__ -prune -exec rm -rf {} +
mkdir -p "$here/out"
tar -C "$stage" -czf "$here/out/iotcloud-bundle.tar.gz" iotcloud-bundle
ls -l "$here/out/iotcloud-bundle.tar.gz"
