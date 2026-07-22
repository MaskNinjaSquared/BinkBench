#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINKPLAYER="${BINKPLAYER_PATH:-${SCRIPT_DIR}/BinkPlayer64}"
HOOKER_SO="${BINKHOOKER_SO:-${SCRIPT_DIR}/bink_hooker.so}"

BK2_PATH=""
OUTPUT_DIR=""
WIDTH=""
HEIGHT=""
MAX_FRAMES_ARG=""
PLAYER_PID=""

usage() {
    cat <<EOF
Usage: $0 --bk2 <bk2-file-or-dir> --output <output-dir> [--width <w> --height <h>] [--frames <n>]

Environment:
  BINKPLAYER_PATH   Path to BinkPlayer64 (default: ./BinkPlayer64)
  BINKHOOKER_SO     Path to bink_hooker.so (default: ./bink_hooker.so)
  BINK_MAX_FRAMES   Override expected total frame count
EOF
    exit 1
}

cleanup() {
    if [[ -n "${PLAYER_PID:-}" ]]; then
        # Recursively kill all child processes spawned under PLAYER_PID (including BinkPlayer64 & Xvfb)
        pkill -P "$PLAYER_PID" 2>/dev/null || true
        kill "$PLAYER_PID" 2>/dev/null || true
        sleep 0.1
        pkill -9 -P "$PLAYER_PID" 2>/dev/null || true
        kill -9 "$PLAYER_PID" 2>/dev/null || true
    fi
    rm -rf "${TMP_DIR:-}"
}
trap cleanup EXIT INT TERM

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bk2)                 BK2_PATH="$2"; shift 2 ;;
        --output)              OUTPUT_DIR="$2"; shift 2 ;;
        --width)               WIDTH="$2"; shift 2 ;;
        --height)              HEIGHT="$2"; shift 2 ;;
        --frames|--max-frames) MAX_FRAMES_ARG="$2"; shift 2 ;;
        -h|--help)             usage ;;
        *) echo "Unknown argument: $1"; usage ;;
    esac
done

[[ -z "${BK2_PATH:-}" || -z "${OUTPUT_DIR:-}" ]] && usage

if [[ -d "$BK2_PATH" ]]; then
    BK2_FILE=$(find "$BK2_PATH" -maxdepth 1 -name "*.bk2" -print -quit)
    [[ -z "$BK2_FILE" ]] && { echo "[decode_wrapper] ERROR: No .bk2 file found in $BK2_PATH"; exit 1; }
    BK2_SEARCH_DIR="$BK2_PATH"
    BK2_PATH="$BK2_FILE"
else
    BK2_SEARCH_DIR="$(dirname "$BK2_PATH")"
fi

[[ -f "$BK2_PATH" ]] || { echo "[decode_wrapper] ERROR: $BK2_PATH not found"; exit 1; }
[[ -x "$BINKPLAYER" ]] || { echo "[decode_wrapper] ERROR: BinkPlayer64 not found at $BINKPLAYER"; exit 1; }
[[ -f "$HOOKER_SO" ]]  || { echo "[decode_wrapper] ERROR: bink_hooker.so not found at $HOOKER_SO"; exit 1; }

mkdir -p "$OUTPUT_DIR"
TMP_DIR=$(mktemp -d)

TOTAL_FRAMES=0
CROP_W="${WIDTH:-0}"
CROP_H="${HEIGHT:-0}"

if [[ -n "${MAX_FRAMES_ARG:-}" && "$MAX_FRAMES_ARG" -gt 0 ]]; then
    TOTAL_FRAMES="$MAX_FRAMES_ARG"
elif [[ -n "${BINK_MAX_FRAMES:-}" && "$BINK_MAX_FRAMES" -gt 0 ]]; then
    TOTAL_FRAMES="$BINK_MAX_FRAMES"
fi

DEMUX_INFO=$(python3 -c '
import struct, json, sys, os

bk2_path = sys.argv[1]
search_dir = sys.argv[2]

frames = 0
width = 0
height = 0

meta_path = os.path.join(search_dir, "meta.json")
if os.path.isfile(meta_path):
    try:
        with open(meta_path) as f:
            d = json.load(f)
            frames = d.get("frame_count") or d.get("frames") or d.get("num_frames") or 0
            width = d.get("width") or 0
            height = d.get("height") or 0
    except Exception:
        pass

if frames <= 0 or width <= 0 or height <= 0:
    try:
        with open(bk2_path, "rb") as f:
            header = f.read(36)
            if len(header) >= 36:
                magic = header[0:4]
                tag = struct.unpack(">I", magic)[0]
                is_bink1 = magic in (b"BIKb", b"BIKi")
                is_bink2 = struct.unpack(">I", b"KB2a")[0] <= tag <= struct.unpack(">I", b"KB2k")[0]
                if is_bink1 or is_bink2:
                    hdr_frames, _, _, hdr_w, hdr_h = struct.unpack_from("<5I", header, offset=8)
                    if frames <= 0: frames = hdr_frames
                    if width <= 0: width = hdr_w
                    if height <= 0: height = hdr_h
    except Exception:
        pass

print(f"{frames} {width} {height}")
' "$BK2_PATH" "$BK2_SEARCH_DIR" 2>/dev/null || echo "0 0 0")

READ_FRAMES=$(echo "$DEMUX_INFO" | awk '{print $1}')
READ_W=$(echo "$DEMUX_INFO" | awk '{print $2}')
READ_H=$(echo "$DEMUX_INFO" | awk '{print $3}')

if [[ "$TOTAL_FRAMES" -eq 0 && -n "$READ_FRAMES" && "$READ_FRAMES" -gt 0 ]]; then
    TOTAL_FRAMES="$READ_FRAMES"
fi

if [[ "$CROP_W" -eq 0 && -n "$READ_W" && "$READ_W" -gt 0 ]]; then
    CROP_W="$READ_W"
fi

if [[ "$CROP_H" -eq 0 && -n "$READ_H" && "$READ_H" -gt 0 ]]; then
    CROP_H="$READ_H"
fi

echo "[decode_wrapper] Decoding:    $(basename "$BK2_PATH")"
echo "[decode_wrapper] Output:      $OUTPUT_DIR"
echo "[decode_wrapper] Resolution:  ${CROP_W}x${CROP_H}"
if [[ "$TOTAL_FRAMES" -gt 0 ]]; then
    echo "[decode_wrapper] Frame count: $TOTAL_FRAMES"
    export BINK_MAX_FRAMES="$TOTAL_FRAMES"
fi

if [[ "$TOTAL_FRAMES" -gt 0 ]]; then
    BINK_DUMP_PNG=1 BINK_DUMP_BMP=0 BINK_DUMP_RAW=0 BINK_DUMP_DIR="$TMP_DIR" \
        BINK_CROP_WIDTH="$CROP_W" BINK_CROP_HEIGHT="$CROP_H" \
        BINK_DEBUG=0 BINK_TRACE=0 BINK_FILTER_WINDOW=0 \
        LD_PRELOAD="$HOOKER_SO" xvfb-run -a --server-args="-screen 0 1920x1080x24+32" \
        "$BINKPLAYER" -l -n -a "$BK2_PATH" >/dev/null 2>&1 &
    PLAYER_PID=$!

    MAX_WAIT=600
    ELAPSED=0
    while kill -0 "$PLAYER_PID" 2>/dev/null; do
        shopt -s nullglob
        frames=("$TMP_DIR"/*.png)
        if [[ ${#frames[@]} -ge "$TOTAL_FRAMES" ]]; then
            break
        fi
        sleep 0.1
        ELAPSED=$((ELAPSED + 1))
        if [[ $ELAPSED -ge $((MAX_WAIT * 10)) ]]; then
            echo "[decode_wrapper] WARNING: Timed out waiting for frames."
            break
        fi
    done
else
    BINK_DUMP_PNG=1 BINK_DUMP_BMP=0 BINK_DUMP_RAW=0 BINK_DUMP_DIR="$TMP_DIR" \
        BINK_CROP_WIDTH="$CROP_W" BINK_CROP_HEIGHT="$CROP_H" \
        BINK_DEBUG=0 BINK_TRACE=0 BINK_FILTER_WINDOW=0 \
        LD_PRELOAD="$HOOKER_SO" xvfb-run -a --server-args="-screen 0 1920x1080x24+32" \
        "$BINKPLAYER" -n -a "$BK2_PATH" 2>/dev/null || true
fi

shopt -s nullglob
frames=("$TMP_DIR"/*.png)
if [[ ${#frames[@]} -eq 0 ]]; then
    echo "[decode_wrapper] ERROR: No frames captured. Decode failed."
    exit 1
fi

mv "$TMP_DIR"/*.png "$OUTPUT_DIR/" 2>/dev/null || true
mv "$TMP_DIR"/*.meta "$OUTPUT_DIR/" 2>/dev/null || true

echo "[decode_wrapper] Successfully saved ${#frames[@]} frames to $OUTPUT_DIR"