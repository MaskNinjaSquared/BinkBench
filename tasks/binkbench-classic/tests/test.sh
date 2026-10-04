#!/usr/bin/env bash
set -uo pipefail

REWARD_PATH="/logs/verifier/reward.json"
mkdir -p "$(dirname "$REWARD_PATH")"

# Held-out clips are .bk2 files baked into the verifier image (hermetic:
# nothing is downloaded at grading time).
export HELD_OUT_DIR="${HELD_OUT_DIR:-/tests/held-out}"

# Artifact transfer may not preserve the executable bit.
chmod +x /output/encoder 2>/dev/null || true

python3 /tests/evaluation.py --held-out "$HELD_OUT_DIR"
EXIT_CODE=$?

# reward.json must hold numbers only (Harbor); the full report is report.json.
if [ ! -f "$REWARD_PATH" ]; then
    echo "[test.sh] evaluation.py did not produce $REWARD_PATH (exit $EXIT_CODE) - writing zero-reward fallback" >&2
    echo '{"reward": 0.0}' > "$REWARD_PATH"
fi

exit 0
