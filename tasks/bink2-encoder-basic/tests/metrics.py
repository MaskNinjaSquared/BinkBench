#!/usr/bin/env python3
"""
tests/metrics.py — BinkBench verifier-side scorer.

Mirrors tools/self_eval.py's exact pipeline (same encoder invocation, same
scoring method) but runs against /tests/held-out/ clips and aggregates into
Harbor's reward.json schema.
"""

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
from PIL import Image
from skimage.metrics import peak_signal_noise_ratio as sk_psnr
from skimage.metrics import structural_similarity as sk_ssim

ENCODER_PATH = "/output/encoder"
BINKPLAYER_PATH = os.environ.get("BINKPLAYER_PATH", "/app/internal/BinkPlayer64")
BINKHOOKER_SO = os.environ.get("BINKHOOKER_SO", "/app/internal/bink_hooker.so")
DECODE_WRAPPER = os.environ.get("DECODE_WRAPPER", "/app/tools/decode_wrapper.sh")
FFMPEG = os.environ.get("BINKBENCH_FFMPEG", "ffmpeg")
HELD_OUT_DIR = Path(os.environ.get("HELD_OUT_DIR", "/tests/held-out"))
REWARD_PATH = Path(os.environ.get("REWARD_PATH", "/logs/verifier/reward.json"))

PSNR_CEILING = 50.0

# Timeout configurations (in seconds)
DEFAULT_ENCODE_TIMEOUT = 1800
DEFAULT_DECODE_TIMEOUT = 360
TOTAL_VERIFIER_TIMEOUT = int(os.environ.get("VERIFIER_TIMEOUT_SEC", 10800))
SHUTDOWN_BUFFER = 60  # Reserve buffer for log aggregation and writing JSON reward


def read_meta(frames_dir: Path):
    meta_path = frames_dir / "meta.json"
    if meta_path.exists():
        with open(meta_path) as f:
            m = json.load(f)
        return m["width"], m["height"], m.get("fps", 30)
    first = sorted(frames_dir.glob("*.png"))[0]
    with Image.open(first) as im:
        w, h = im.size
    return w, h, 30


def run_encoder(frames_dir: Path, out_bk2: Path, width: int, height: int, fps: int, timeout: int):
    subprocess.run(
        [ENCODER_PATH, "--input", str(frames_dir), "--output", str(out_bk2),
         "--width", str(width), "--height", str(height), "--fps", str(fps)],
        check=True, timeout=timeout,
    )


def run_decode_wrapper(bk2_path: Path, out_dir: Path, timeout: int) -> bool:
    try:
        result = subprocess.run(
            [DECODE_WRAPPER, "--bk2", str(bk2_path), "--output", str(out_dir)],
            capture_output=True, text=True, timeout=timeout,
            env={**os.environ, "BINKPLAYER_PATH": BINKPLAYER_PATH, "BINKHOOKER_SO": BINKHOOKER_SO},
        )
        if result.returncode != 0:
            print(f"[metrics] decode_wrapper.sh failed for {bk2_path.name}:\n{result.stderr}",
                  file=sys.stderr)
            return False
        return len(list(out_dir.glob("*.png"))) > 0
    except subprocess.TimeoutExpired:
        print(f"[metrics] decode_wrapper.sh timed out for {bk2_path.name} after {timeout}s",
              file=sys.stderr)
        return False


def load_frames(frame_dir: Path):
    paths = sorted(frame_dir.glob("*.png"))
    return [np.array(Image.open(p).convert("RGB")) for p in paths]


def compute_psnr_ssim(source_frames, decoded_frames):
    n = min(len(source_frames), len(decoded_frames))
    if len(source_frames) != len(decoded_frames):
        print(f"[metrics] WARNING: frame count mismatch "
              f"(source={len(source_frames)} decoded={len(decoded_frames)}), "
              f"scoring first {n}", file=sys.stderr)
    psnr_vals, ssim_vals = [], []
    for i in range(n):
        s, d = source_frames[i], decoded_frames[i]
        if s.shape != d.shape:
            raise ValueError(f"Frame {i} shape mismatch: {s.shape} vs {d.shape}")
        psnr_vals.append(sk_psnr(s, d, data_range=255))
        ssim_vals.append(sk_ssim(s, d, channel_axis=2, data_range=255))
    return {
        "psnr": float(np.mean(psnr_vals)),
        "psnr_min": float(np.min(psnr_vals)),
        "ssim": float(np.mean(ssim_vals)),
        "frames_scored": n,
    }


def compute_vmaf(source_dir: Path, decoded_dir: Path, fps: int, timeout: int):
    vmaf_log = Path(tempfile.mktemp(suffix=".json"))
    cmd = [
        FFMPEG, "-y",
        "-framerate", str(fps), "-i", str(source_dir / "frame_%06d.png"),
        "-framerate", str(fps), "-i", str(decoded_dir / "frame_%06d.png"),
        "-lavfi", f"libvmaf=log_path={vmaf_log}:log_fmt=json",
        "-f", "null", "-",
    ]
    try:
        subprocess.run(cmd, capture_output=True, timeout=timeout, check=True)
        with open(vmaf_log) as f:
            data = json.load(f)
        pooled = data.get("pooled_metrics", data.get("pooled", {}))
        vmaf_block = pooled.get("vmaf", {})
        return float(vmaf_block.get("mean", vmaf_block.get("harmonic_mean")))
    except Exception as e:
        print(f"[metrics] VMAF failed, excluding from geomean: {e}", file=sys.stderr)
        return None
    finally:
        vmaf_log.unlink(missing_ok=True)


def geomean(psnr, ssim, vmaf):
    psnr_norm = min(psnr / PSNR_CEILING, 1.0)
    if vmaf is None:
        return (psnr_norm * ssim) ** 0.5
    return (psnr_norm * ssim * (vmaf / 100.0)) ** (1.0 / 3.0)


def score_clip(frames_dir: Path, verifier_start_time: float):
    clip_name = frames_dir.name
    out_bk2 = Path(f"/tmp/verifier_output_{clip_name}.bk2")

    try:
        width, height, fps = read_meta(frames_dir)
        raw_png_size = sum(p.stat().st_size for p in frames_dir.glob("*.png"))

        # Calculate remaining budget for encoding
        elapsed = time.time() - verifier_start_time
        remaining_budget = TOTAL_VERIFIER_TIMEOUT - elapsed - SHUTDOWN_BUFFER
        if remaining_budget <= 0:
            print(f"[metrics] Budget exhausted before encoding {clip_name}", file=sys.stderr)
            return {"clip": clip_name, "decode_failed": True, "reason": "verifier_budget_exhausted"}

        encode_timeout = min(DEFAULT_ENCODE_TIMEOUT, max(1, int(remaining_budget)))

        try:
            run_encoder(frames_dir, out_bk2, width, height, fps, encode_timeout)
        except subprocess.TimeoutExpired:
            return {"clip": clip_name, "decode_failed": True, "reason": "encode_timeout"}
        except subprocess.CalledProcessError as e:
            return {"clip": clip_name, "decode_failed": True, "reason": f"encoder_exit_{e.returncode}"}

        if not out_bk2.exists():
            return {"clip": clip_name, "decode_failed": True, "reason": "no_output_file"}

        # Calculate remaining budget for decoding
        elapsed = time.time() - verifier_start_time
        remaining_budget = TOTAL_VERIFIER_TIMEOUT - elapsed - SHUTDOWN_BUFFER
        if remaining_budget <= 0:
            print(f"[metrics] Budget exhausted before decoding {clip_name}", file=sys.stderr)
            return {"clip": clip_name, "decode_failed": True, "reason": "verifier_budget_exhausted"}

        decode_timeout = min(DEFAULT_DECODE_TIMEOUT, max(1, int(remaining_budget)))

        with tempfile.TemporaryDirectory(prefix="metrics_decoded_") as tmp:
            decoded_dir = Path(tmp)
            if not run_decode_wrapper(out_bk2, decoded_dir, decode_timeout):
                return {"clip": clip_name, "decode_failed": True, "reason": "decode_failed"}

            source_frames = load_frames(frames_dir)
            decoded_frames = load_frames(decoded_dir)

            try:
                quality = compute_psnr_ssim(source_frames, decoded_frames)
            except ValueError as e:
                return {"clip": clip_name, "decode_failed": True, "reason": str(e)}

            elapsed = time.time() - verifier_start_time
            remaining_budget = TOTAL_VERIFIER_TIMEOUT - elapsed - SHUTDOWN_BUFFER
            vmaf_timeout = min(DEFAULT_DECODE_TIMEOUT, max(1, int(remaining_budget))) if remaining_budget > 0 else 1

            vmaf = compute_vmaf(frames_dir, decoded_dir, fps, vmaf_timeout)

            encoded_size = out_bk2.stat().st_size
            bpp = (encoded_size * 8) / (width * height * quality["frames_scored"])

            return {
                "clip": clip_name,
                "decode_failed": False,
                "frame_count": quality["frames_scored"],
                "compression_ratio": round(encoded_size / raw_png_size, 4) if raw_png_size else None,
                "bpp": round(bpp, 5),
                "psnr": round(quality["psnr"], 3),
                "psnr_min": round(quality["psnr_min"], 3),
                "ssim": round(quality["ssim"], 4),
                "vmaf": round(vmaf, 3) if vmaf is not None else None,
                "quality_geomean": round(geomean(quality["psnr"], quality["ssim"], vmaf), 4),
            }

    finally:
        out_bk2.unlink(missing_ok=True)


def main():
    verifier_start_time = time.time()

    if not Path(ENCODER_PATH).exists():
        reward = {"reward": 0.0, "error": "no_encoder_at_/output/encoder", "clips": []}
        REWARD_PATH.parent.mkdir(parents=True, exist_ok=True)
        with open(REWARD_PATH, "w") as f:
            json.dump(reward, f, indent=2)
        print(json.dumps(reward, indent=2))
        sys.exit(1)

    clip_dirs = sorted([d for d in HELD_OUT_DIR.iterdir() if d.is_dir()])
    if not clip_dirs:
        print(f"[metrics] ERROR: no clips found in {HELD_OUT_DIR}", file=sys.stderr)
        sys.exit(1)

    results = []
    for clip_dir in clip_dirs:
        print(f"[metrics] scoring {clip_dir.name}...", file=sys.stderr)
        results.append(score_clip(clip_dir, verifier_start_time))

    # Failed clips score as zero, not excluded — matches instruction.md's
    # stated consequence ("that clip will score as a failure") literally.
    geomeans = [r["quality_geomean"] if not r.get("decode_failed", True) else 0.0 for r in results]
    mean_quality_geomean = float(np.mean(geomeans)) if geomeans else 0.0

    successful = [r for r in results if not r.get("decode_failed", True)]
    mean_bpp = float(np.mean([r["bpp"] for r in successful])) if successful else None
    mean_compression_ratio = (
        float(np.mean([r["compression_ratio"] for r in successful if r.get("compression_ratio") is not None]))
        if successful else None
    )

    n_failed = sum(1 for r in results if r.get("decode_failed", True))

    reward = {
        "reward": round(mean_quality_geomean, 4),
        "mean_quality_geomean": round(mean_quality_geomean, 4),
        "mean_bpp": round(mean_bpp, 5) if mean_bpp is not None else None,
        "mean_compression_ratio": round(mean_compression_ratio, 4) if mean_compression_ratio is not None else None,
        "clips_total": len(results),
        "clips_failed": n_failed,
        "clips": results,
    }

    REWARD_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(REWARD_PATH, "w") as f:
        json.dump(reward, f, indent=2)

    print(json.dumps(reward, indent=2))


if __name__ == "__main__":
    main()