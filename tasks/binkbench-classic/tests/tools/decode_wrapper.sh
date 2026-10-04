#!/bin/bash
# decode_wrapper.sh - decode a .bk2 to PNG frames via BinkPlayer64 + bink_hooker.so
#
# The .bk2 header is the single source of truth for frame count and display
# resolution. --width/--height, if given, are only ASSERTIONS: the decode
# fails (exit 2) if the header disagrees. They never override the header.
#
# Exit codes:
#   0  success
#   1  bad input, unreadable/invalid header, or no frames captured
#   2  header resolution does not match --width/--height
#   3  fewer frames captured than the header declares (frames are still saved)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINKPLAYER="${BINKPLAYER_PATH:-${SCRIPT_DIR}/BinkPlayer64}"
HOOKER_SO="${BINKHOOKER_SO:-${SCRIPT_DIR}/bink_hooker.so}"
# Seconds to wait for frames. Keep this below any caller-side subprocess
# timeout so the cleanup trap gets to run (a SIGKILL from the caller skips it).
DECODE_TIMEOUT="${BINK_DECODE_TIMEOUT:-300}"

BK2_PATH=""
OUTPUT_DIR=""
EXPECT_W=""
EXPECT_H=""
MAX_FRAMES_ARG=""
PLAYER_PID=""
TMP_DIR=""

usage() {
    cat <<EOF
Usage: $0 --bk2 <bk2-file-or-dir> --output <output-dir> [--width <w> --height <h>] [--frames <n>]

  --width/--height   Assert the header's display resolution (never overrides it).
  --frames           Override the frame count from the header.

Environment:
  BINKPLAYER_PATH       Path to BinkPlayer64 (default: ./BinkPlayer64)
  BINKHOOKER_SO         Path to bink_hooker.so (default: ./bink_hooker.so)
  BINK_MAX_FRAMES       Override expected total frame count
  BINK_DECODE_TIMEOUT   Seconds to wait for frames (default: 300)
EOF
    exit 1
}

stop_player() {
    if [[ -n "${PLAYER_PID:-}" ]]; then
        # Kill everything spawned under PLAYER_PID (BinkPlayer64 and Xvfb)
        pkill -P "$PLAYER_PID" 2>/dev/null || true
        kill "$PLAYER_PID" 2>/dev/null || true
        sleep 0.1
        pkill -9 -P "$PLAYER_PID" 2>/dev/null || true
        kill -9 "$PLAYER_PID" 2>/dev/null || true
        wait "$PLAYER_PID" 2>/dev/null || true
        PLAYER_PID=""
    fi
}

cleanup() {
    stop_player
    rm -rf "${TMP_DIR:-}"
}
trap cleanup EXIT INT TERM

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bk2)                 BK2_PATH="$2"; shift 2 ;;
        --output)              OUTPUT_DIR="$2"; shift 2 ;;
        --width)               EXPECT_W="$2"; shift 2 ;;
        --height)              EXPECT_H="$2"; shift 2 ;;
        --frames|--max-frames) MAX_FRAMES_ARG="$2"; shift 2 ;;
        -h|--help)             usage ;;
        *) echo "Unknown argument: $1"; usage ;;
    esac
done

[[ -z "${BK2_PATH:-}" || -z "${OUTPUT_DIR:-}" ]] && usage

if [[ -d "$BK2_PATH" ]]; then
    BK2_FILE=$(find "$BK2_PATH" -maxdepth 1 -name "*.bk2" | sort | head -n 1 || true)
    [[ -z "$BK2_FILE" ]] && { echo "[decode_wrapper] ERROR: No .bk2 file found in $BK2_PATH"; exit 1; }
    BK2_PATH="$BK2_FILE"
fi

[[ -f "$BK2_PATH" ]] || { echo "[decode_wrapper] ERROR: $BK2_PATH not found"; exit 1; }
[[ -x "$BINKPLAYER" ]] || { echo "[decode_wrapper] ERROR: BinkPlayer64 not found at $BINKPLAYER"; exit 1; }
[[ -f "$HOOKER_SO" ]]  || { echo "[decode_wrapper] ERROR: bink_hooker.so not found at $HOOKER_SO"; exit 1; }

# ---- Header: single source of truth --------------------------------------
# Layout: 0 magic | 4 file size | 8 frames | 12 largest frame | 16 frames | 20 width | 24 height
HDR_INFO=$(python3 - "$BK2_PATH" <<'PY' 2>/dev/null || echo "ERR python_failed"
import struct, sys
try:
    with open(sys.argv[1], "rb") as f:
        h = f.read(28)
    if len(h) < 28:
        print("ERR header_truncated")
        sys.exit(0)
    magic = h[:4]
    ok = magic in (b"BIKb", b"BIKi") or (b"KB2a" <= magic <= b"KB2k")
    if not ok:
        print("ERR unrecognized_magic_%s" % magic.hex())
        sys.exit(0)
    frames = struct.unpack_from("<I", h, 8)[0]
    w, hgt = struct.unpack_from("<2I", h, 20)
    if frames <= 0 or w <= 0 or hgt <= 0:
        print("ERR zero_field_frames=%d_size=%dx%d" % (frames, w, hgt))
        sys.exit(0)
    print("OK %d %d %d %s" % (frames, w, hgt, magic.decode("latin-1")))
except Exception as e:
    print("ERR %s" % e)
PY
)

read -r HDR_STATUS HDR_REST <<<"$HDR_INFO"
if [[ "$HDR_STATUS" != "OK" ]]; then
    echo "[decode_wrapper] ERROR: no valid Bink header in $(basename "$BK2_PATH"): ${HDR_REST:-unknown}"
    exit 1
fi
read -r HDR_FRAMES HDR_W HDR_H HDR_MAGIC <<<"$HDR_REST"

if { [[ -n "$EXPECT_W" && "$EXPECT_W" != "$HDR_W" ]]; } || { [[ -n "$EXPECT_H" && "$EXPECT_H" != "$HDR_H" ]]; }; then
    echo "[decode_wrapper] ERROR: header declares ${HDR_W}x${HDR_H}, expected ${EXPECT_W:-?}x${EXPECT_H:-?}"
    exit 2
fi

TOTAL_FRAMES="$HDR_FRAMES"
if [[ -n "${MAX_FRAMES_ARG:-}" && "$MAX_FRAMES_ARG" -gt 0 ]]; then
    TOTAL_FRAMES="$MAX_FRAMES_ARG"
elif [[ -n "${BINK_MAX_FRAMES:-}" && "$BINK_MAX_FRAMES" -gt 0 ]]; then
    TOTAL_FRAMES="$BINK_MAX_FRAMES"
fi

mkdir -p "$OUTPUT_DIR"
TMP_DIR=$(mktemp -d)

echo "[decode_wrapper] Decoding:    $(basename "$BK2_PATH") ($HDR_MAGIC)"
echo "[decode_wrapper] Output:      $OUTPUT_DIR"
echo "[decode_wrapper] Resolution:  ${HDR_W}x${HDR_H} (from header)"
echo "[decode_wrapper] Frame count: $TOTAL_FRAMES"
export BINK_MAX_FRAMES="$TOTAL_FRAMES"

# The virtual screen must cover the native decoded size; the hooker crops
# (top-left) from the native buffer down to the header's display size.
BINK_DUMP_PNG=1 BINK_DUMP_BMP=0 BINK_DUMP_RAW=0 BINK_DUMP_DIR="$TMP_DIR" \
    BINK_CROP_WIDTH="$HDR_W" BINK_CROP_HEIGHT="$HDR_H" \
    BINK_DEBUG=0 BINK_TRACE=0 BINK_FILTER_WINDOW=0 \
    LD_PRELOAD="$HOOKER_SO" xvfb-run -a --server-args="-screen 0 ${HDR_W}x${HDR_H}x24+32" \
    "$BINKPLAYER" -l -n -a "$BK2_PATH" >/dev/null 2>&1 &
PLAYER_PID=$!

count_pngs() {
    find "$TMP_DIR" -maxdepth 1 -name '*.png' | wc -l
}

# The hooker writes PNGs in place (no temp file + rename), so a file can exist
# while still being written. Wait until total PNG size stops changing.
wait_for_stable_files() {
    local prev="" cur same=0 i
    for i in $(seq 1 100); do
        cur=$(find "$TMP_DIR" -maxdepth 1 -name '*.png' -printf '%s\n' | awk '{s+=$1} END{print s+0}')
        if [[ "$cur" == "$prev" ]]; then
            same=$((same + 1))
        else
            same=0
        fi
        if [[ $same -ge 2 ]]; then
            return 0
        fi
        prev="$cur"
        sleep 0.3
    done
    return 0
}

MAX_ITERS=$((DECODE_TIMEOUT * 10))
ITER=0
while kill -0 "$PLAYER_PID" 2>/dev/null; do
    if [[ $(count_pngs) -ge "$TOTAL_FRAMES" ]]; then
        break
    fi
    sleep 0.1
    ITER=$((ITER + 1))
    if [[ $ITER -ge $MAX_ITERS ]]; then
        echo "[decode_wrapper] WARNING: timed out after ${DECODE_TIMEOUT}s waiting for frames."
        break
    fi
done

# Let the last PNG finish, then stop the player so nothing writes while we move files.
wait_for_stable_files
stop_player

mapfile -t ALL_FRAMES < <(find "$TMP_DIR" -maxdepth 1 -name '*.png' | sort)
N_CAPTURED=${#ALL_FRAMES[@]}
if [[ $N_CAPTURED -eq 0 ]]; then
    echo "[decode_wrapper] ERROR: No frames captured. Decode failed."
    exit 1
fi

# -l can let the player run past the last frame before we stop it; keep exactly
# the declared number so callers can compare frame counts strictly.
if [[ $N_CAPTURED -gt $TOTAL_FRAMES ]]; then
    for f in "${ALL_FRAMES[@]:$TOTAL_FRAMES}"; do
        rm -f "$f" "${f%.png}.meta" "$f.meta"
    done
    N_CAPTURED=$TOTAL_FRAMES
fi

mv "$TMP_DIR"/*.png "$OUTPUT_DIR/"
mv "$TMP_DIR"/*.meta "$OUTPUT_DIR/" 2>/dev/null || true

echo "[decode_wrapper] Successfully saved ${N_CAPTURED} frames to $OUTPUT_DIR"
if [[ $N_CAPTURED -lt $TOTAL_FRAMES ]]; then
    echo "[decode_wrapper] ERROR: captured ${N_CAPTURED} of ${TOTAL_FRAMES} frames declared by the header."
    exit 3
fi