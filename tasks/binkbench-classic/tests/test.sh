#!/usr/bin/env bash
set -uo pipefail

REWARD_PATH="/logs/verifier/reward.json"
mkdir -p "$(dirname "$REWARD_PATH")"

# Prefer the held-out clips baked into the verifier image (hermetic grading).
# Fall back to a runtime download only if the image was built without them, so
# a standalone metrics.py run still works outside the image.
HELD_OUT_BAKED="/tests/held-out"
if [ -d "$HELD_OUT_BAKED" ]; then
    export HELD_OUT_DIR="$HELD_OUT_BAKED"
else
    HELD_OUT_BASE="/tmp/BinkBenchAssets"
    if [ ! -d "$HELD_OUT_BASE/held-out" ]; then
        rm -rf "$HELD_OUT_BASE"
        mkdir -p "$HELD_OUT_BASE"
        wget -q https://huggingface.co/datasets/MaskNinja/BinkBenchAssets/resolve/main/held-out.tar.gz -O /tmp/held-out.tar.gz
        tar xzf /tmp/held-out.tar.gz -C "$HELD_OUT_BASE"
        rm /tmp/held-out.tar.gz
    fi
    export HELD_OUT_DIR="$HELD_OUT_BASE/held-out"
fi

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