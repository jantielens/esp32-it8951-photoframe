#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$SCRIPT_DIR"

if [[ -f "$APP_DIR/.env" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "$APP_DIR/.env"
  set +a
fi

: "${AZ_RG:?Set AZ_RG to the resource group name}"
: "${AZ_APP:?Set AZ_APP to the App Service name}"

if command -v az >/dev/null 2>&1; then
  echo "Using Azure CLI: $(az version | head -n 1)"
else
  echo "Azure CLI not found. Install it first."
  exit 1
fi

if [[ -n "${AZ_SUB:-}" ]]; then
  az account set --subscription "$AZ_SUB"
fi

ZIP_DIR="$(mktemp -d)"
ZIP_PATH="$ZIP_DIR/web-poc.zip"

pushd "$APP_DIR" >/dev/null
zip -r "$ZIP_PATH" . -x "__pycache__/*" "*.pyc" ".venv/*"
popd >/dev/null

az webapp deploy \
  --resource-group "$AZ_RG" \
  --name "$AZ_APP" \
  --src-path "$ZIP_PATH"

echo "Deploy complete."
