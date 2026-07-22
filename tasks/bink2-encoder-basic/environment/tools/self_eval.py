#!/bin/env python3
"""
self_eval.py — BinkBench agent-facing self-evaluation tool.

Usage:
  self_eval.py --frames <source frames dir>

Encodes via /output/encoder, decodes via decode_wrapper.sh (BinkPlayer64 +
bink_hooker.so), scores against the source frames, and prints JSON.
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image
from skimage.metrics import peak_signal_noise_ratio as sk_psnr
from skimage.metrics import structural_similarity as sk_ssim

ENCODER_PATH = "/output/encoder"
FFMPEG = os.environ.get("BINKBENCH_FFMPEG", "ffmpeg")
PSNR_CEILING = 50.0
ENCODE_TIMEOUT = 900
DECODE_TIMEOUT = 180


def read_meta(frames_dir: Path):
    meta_path = frames_dir / "meta.json"
    if meta_path.exists():
        with open(meta_path) as f:
            m = json.load(f)
        return m["width"], m["height"], m.get("fps", 30)
    # fallback: infer from first frame, assume 30fps
    first = sorted(frames_dir.glob("*.png"))[0]
    with Image.open(first) as im:
        w, h = im.size
    return w, h, 30


def run_encoder(frames_dir: Path, out_bk2: Path, width: int, height: int, fps: int):
    subprocess.run(
        [ENCODER_PATH, "--input", str(frames_dir), "--output", str(out_bk2),
         "--width", str(width), "--height", str(height), "--fps", str(fps)],
        check=True, timeout=ENCODE_TIMEOUT,
    )


def run_decode_wrapper(bk2_path: Path, out_dir: Path, width: int, height: int) -> bool:
    script_dir = Path(__file__).parent.resolve()
    wrapper = script_dir / "decode_wrapper.sh"
    result = subprocess.run(
        [str(wrapper), "--bk2", str(bk2_path), "--output", str(out_dir),
         "--width", str(width), "--height", str(height)],
        capture_output=True, text=True, timeout=DECODE_TIMEOUT,
    )
    if result.returncode != 0:
        print(f"[self_eval] decode_wrapper.sh failed:\n{result.stderr}", file=sys.stderr)
        return False
    frames = sorted(out_dir.glob("*.png"))
    return len(frames) > 0


def load_frames(frame_dir: Path):
    paths = sorted(frame_dir.glob("*.png"))
    return [np.array(Image.open(p).convert("RGB")) for p in paths]


def compute_psnr_ssim(source_frames, decoded_frames):
    n = min(len(source_frames), len(decoded_frames))
    if len(source_frames) != len(decoded_frames):
        print(f"[self_eval] WARNING: frame count mismatch "
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


def compute_vmaf(source_dir: Path, decoded_dir: Path, fps: int, n_frames: int):
    vmaf_log = Path(tempfile.mktemp(suffix=".json"))
    cmd = [
        FFMPEG, "-y",
        "-framerate", str(fps), "-i", str(source_dir / "frame_%06d.png"),
        "-framerate", str(fps), "-i", str(decoded_dir / "frame_%06d.png"),
        "-lavfi", f"libvmaf=log_path={vmaf_log}:log_fmt=json",
        "-f", "null", "-",
    ]
    try:
        subprocess.run(cmd, capture_output=True, timeout=DECODE_TIMEOUT, check=True)
        with open(vmaf_log) as f:
            data = json.load(f)
        pooled = data.get("pooled_metrics", data.get("pooled", {}))
        vmaf_block = pooled.get("vmaf", {})
        return float(vmaf_block.get("mean", vmaf_block.get("harmonic_mean")))
    except Exception as e:
        print(f"[self_eval] VMAF failed, excluding from geomean: {e}", file=sys.stderr)
        return None
    finally:
        vmaf_log.unlink(missing_ok=True)


def geomean(psnr, ssim, vmaf):
    psnr_norm = min(psnr / PSNR_CEILING, 1.0)
    if vmaf is None:
        return (psnr_norm * ssim) ** 0.5
    return (psnr_norm * ssim * (vmaf / 100.0)) ** (1.0 / 3.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", required=True)
    args = ap.parse_args()

    frames_dir = Path(args.frames)
    clip_name = frames_dir.name
    out_bk2 = Path(f"/tmp/output_{clip_name}.bk2")

    if not Path(ENCODER_PATH).exists():
        print(json.dumps({"error": "no_encoder_at_/output/encoder"}))
        sys.exit(1)

    width, height, fps = read_meta(frames_dir)
    raw_png_size = sum(p.stat().st_size for p in frames_dir.glob("*.png"))

    try:
        run_encoder(frames_dir, out_bk2, width, height, fps)
    except subprocess.TimeoutExpired:
        print(json.dumps({"clip": clip_name, "decode_failed": True, "reason": "encode_timeout"}))
        sys.exit(1)
    except subprocess.CalledProcessError as e:
        print(json.dumps({"clip": clip_name, "decode_failed": True,
                           "reason": f"encoder_exit_{e.returncode}"}))
        sys.exit(1)

    if not out_bk2.exists():
        print(json.dumps({"clip": clip_name, "decode_failed": True, "reason": "no_output_file"}))
        sys.exit(1)

    with tempfile.TemporaryDirectory(prefix="self_eval_decoded_") as tmp:
        decoded_dir = Path(tmp)
        if not run_decode_wrapper(out_bk2, decoded_dir, width, height):
            print(json.dumps({"clip": clip_name, "decode_failed": True, "reason": "decode_failed"}))
            sys.exit(1)

        source_frames = load_frames(frames_dir)
        decoded_frames = load_frames(decoded_dir)

        try:
            quality = compute_psnr_ssim(source_frames, decoded_frames)
        except ValueError as e:
            print(json.dumps({"clip": clip_name, "decode_failed": True, "reason": str(e)}))
            sys.exit(1)

        vmaf = compute_vmaf(frames_dir, decoded_dir, fps, quality["frames_scored"])

        encoded_size = out_bk2.stat().st_size
        bpp = (encoded_size * 8) / (width * height * quality["frames_scored"])

        result = {
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
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()