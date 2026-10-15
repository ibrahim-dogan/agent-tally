#!/bin/sh
# AgentTally bridge installer (macOS / Linux).
#   curl -fsSL https://raw.githubusercontent.com/ibrahim-dogan/agent-tally/main/host/install.sh | sh
# Options are passed through, e.g. `sh -s -- --no-service`.
set -e
REPO=${AGENTTALLY_REPO:-ibrahim-dogan/agent-tally}
REF=${AGENTTALLY_REF:-main}
command -v python3 >/dev/null 2>&1 || { echo "AgentTally needs python3 (3.8 or newer)." >&2; exit 1; }
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
curl -fsSL "https://raw.githubusercontent.com/$REPO/$REF/host/agenttally.py" -o "$TMP"
python3 "$TMP" install "$@"
