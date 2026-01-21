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

: "${AZ_RG:?Set AZ_RG}" 
: "${AZ_APP:?Set AZ_APP}" 
: "${AZ_LOCATION:?Set AZ_LOCATION}" 

if command -v az >/dev/null 2>&1; then
  echo "Azure CLI OK"
else
  echo "Azure CLI not found. Install it first." >&2
  exit 1
fi

if [[ -n "${AZ_SUB:-}" ]]; then
  az account set --subscription "$AZ_SUB"
fi

PLAN_NAME="${AZ_APP}-plan"

echo "Creating resource group..."
az group create -n "$AZ_RG" -l "$AZ_LOCATION" >/dev/null

echo "Creating App Service plan..."
az appservice plan create -g "$AZ_RG" -n "$PLAN_NAME" --sku B1 --is-linux >/dev/null

echo "Creating Web App..."
az webapp create -g "$AZ_RG" -p "$PLAN_NAME" -n "$AZ_APP" --runtime "PYTHON|3.11" >/dev/null

echo "Configuring app settings..."
az webapp config appsettings set -g "$AZ_RG" -n "$AZ_APP" --settings \
  SCM_DO_BUILD_DURING_DEPLOYMENT=1 \
  WEBSITES_PORT=8000 \
  ENV=prod >/dev/null

az webapp config set -g "$AZ_RG" -n "$AZ_APP" --startup-file "python -m uvicorn app:app --host 0.0.0.0 --port 8000" >/dev/null

echo "Deploying app..."
"$APP_DIR/deploy.sh"

echo "Provision + deploy complete."