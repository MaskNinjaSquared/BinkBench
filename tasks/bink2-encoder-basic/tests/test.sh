#!/usr/bin/env bash
# tests/test.sh — Harbor verifier entrypoint. Runs metrics.py against the
# held-out clips and writes /logs/verifier/reward.json. Falls back to a
# zero-reward file if metrics.py crashes entirely, so a broken verifier
# never silently produces no reward file at all.

set -uo pipefail

REWARD_PATH="/logs/verifier/reward.json"
mkdir -p "$(dirname "$REWARD_PATH")"

python3 /tests/metrics.py
EXIT_CODE=$?

if [ ! -f "$REWARD_PATH" ]; then
    echo "[test.sh] metrics.py did not produce $REWARD_PATH — writing zero-reward fallback" >&2
    cat > "$REWARD_PATH" <<EOF
{
  "reward": 0.0,
  "error": "metrics.py_crashed_or_produced_no_reward_file",
  "exit_code": $EXIT_CODE,
  "clips": []
}
EOF
fi

exit 0