#!/usr/bin/env bash
set -uo pipefail

REWARD_PATH="/logs/verifier/reward.json"
mkdir -p "$(dirname "$REWARD_PATH")"

HELD_OUT_BASE="/tmp/BinkBenchAssets"
if [ ! -d "$HELD_OUT_BASE/held-out" ]; then
    rm -rf "$HELD_OUT_BASE"
    mkdir -p "$HELD_OUT_BASE"
    wget -q https://huggingface.co/datasets/MaskNinja/BinkBenchAssets/resolve/main/held-out.tar.gz -O /tmp/held-out.tar.gz
    tar xzf /tmp/held-out.tar.gz -C "$HELD_OUT_BASE"
    rm /tmp/held-out.tar.gz
fi
export HELD_OUT_DIR="$HELD_OUT_BASE/held-out"

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