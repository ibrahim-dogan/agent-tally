#!/bin/sh
# AgentTally bridge installer (macOS / Linux).
#   curl -fsSL https://raw.githubusercontent.com/ibrahim-dogan/agent-tally/main/host/install.sh | sh
# Options are passed through, e.g. `sh -s -- --no-service`.
set -e
# wrapped in a function so a cut-off download runs nothing
main() {
  REPO=${AGENTTALLY_REPO:-ibrahim-dogan/agent-tally}
  REF=${AGENTTALLY_REF:-main}
  command -v python3 >/dev/null 2>&1 || { echo "AgentTally needs python3 (3.8 or newer)." >&2; exit 1; }
  DIR=$(mktemp -d)
  trap 'rm -rf "$DIR"' EXIT
  curl -fsSL "https://raw.githubusercontent.com/$REPO/$REF/host/agenttally.py" -o "$DIR/agenttally.py"
  # optional: mouse-on-the-display helper (macOS, compiled locally on first use)
  curl -fsSL "https://raw.githubusercontent.com/$REPO/$REF/host/pointer.swift" -o "$DIR/pointer.swift" || true
  python3 "$DIR/agenttally.py" install "$@"
}
main "$@"
