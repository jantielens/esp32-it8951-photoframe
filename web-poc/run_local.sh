#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [[ -f .env ]]; then
  set -a
  # shellcheck disable=SC1091
  source .env
  set +a
fi

ENV_MODE="${ENV:-dev}"
DEV_USER_VALUE="${DEV_USER:-dev@example.com}"
HOST_VALUE="${HOST:-127.0.0.1}"
PORT_VALUE="${PORT:-8000}"

if [[ ! -d .venv ]]; then
  python3 -m venv .venv
fi

source .venv/bin/activate
pip install -r requirements.txt

export ENV="$ENV_MODE"
export DEV_USER="$DEV_USER_VALUE"

exec uvicorn app:app --host "$HOST_VALUE" --port "$PORT_VALUE" --reload