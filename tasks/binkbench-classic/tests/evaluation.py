#!/usr/bin/env python3
"""
evaluation.py - BinkBench unified evaluation and scoring engine.
(Replaces self_eval.py and metrics.py. Keep byte-identical in both images.)

Agent mode (inside the task container):
    evaluation.py --clip /app/samples/cgi/Sample_A.bk2
    evaluation.py --clip <directory of your own PNG frames>
    evaluation.py --clip <ref.bk2> --decode-only     # only extract reference frames

  The reference .bk2 is decoded ONCE with decode_wrapper.sh into PNG frames
  (cached under /tmp/binkbench_ref_cache/). Those frames are what your encoder
  receives as --input and what your output is scored against, exactly as the
  grader does it. Prints a JSON result.

Verifier mode (grader):
    evaluation.py --held-out /tests/held-out         # writes reward.json
    (no arguments: uses $HELD_OUT_DIR, default /tests/held-out, if it exists)
"""

import argparse
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image
from skimage.metrics import peak_signal_noise_ratio as sk_psnr
from skimage.metrics import structural_similarity as sk_ssim

# --------------------------------------------------------------------------- #
# Constants & defaults
# --------------------------------------------------------------------------- #

REQUIRED_MAGIC = b"KB2i"
PSNR_CEILING = 50.0
BPP_REFERENCE = 0.5

DEFAULT_ENCODER = os.environ.get("ENCODER_PATH", "/output/encoder")
DEFAULT_WRAPPER = os.environ.get("DECODE_WRAPPER", "/app/tools/decode_wrapper.sh")
FFMPEG = os.environ.get("BINKBENCH_FFMPEG", "ffmpeg")
# Explicit model so the score never depends on a build's default model.
VMAF_FILTER = "libvmaf=model=version=vmaf_v0.6.1"

HELD_OUT_DIR = Path(os.environ.get("HELD_OUT_DIR", "/tests/held-out"))
REWARD_PATH = Path(os.environ.get("REWARD_PATH", "/logs/verifier/reward.json"))
REF_CACHE_DIR = Path(os.environ.get("BINKBENCH_REF_CACHE", "/tmp/binkbench_ref_cache"))

# Per-stage timeouts (seconds). Agent limits are intentionally half of the
# verifier's encode/decode/vmaf limits, so a clip that fits here has 2x headroom
# at grading. Reference decode is not graded time, so it gets the full limit.
AGENT_LIMITS = {"ref": 360, "encode": 900, "decode": 180, "vmaf": 180}
VERIFIER_LIMITS = {"ref": 360, "encode": 1800, "decode": 360, "vmaf": 360}
TOTAL_VERIFIER_TIMEOUT = int(os.environ.get("VERIFIER_TIMEOUT_SEC", 10800))
SHUTDOWN_BUFFER = 60

# Player locations: honour the environment, else fall back to the image layout.
for _var, _path in (("BINKPLAYER_PATH", "/app/internal/BinkPlayer64"),
                    ("BINKHOOKER_SO", "/app/internal/bink_hooker.so")):
    if _var not in os.environ and Path(_path).exists():
        os.environ[_var] = _path


class ReferenceDecodeError(Exception):
    """The reference .bk2 could not be decoded to the frame count its header declares."""


class BudgetExhausted(Exception):
    """The verifier's overall time budget ran out."""


# --------------------------------------------------------------------------- #
# Helpers, budgets and process-group management
# --------------------------------------------------------------------------- #

def clip_id(bk2_path, root=None):
    """Stable clip name. Files directly under `root` use their stem; others are
    prefixed with their folder (cgi/Sample_A.bk2 vs real/Sample_A.bk2)."""
    p = Path(bk2_path)
    if root is not None and p.parent == Path(root):
        return p.stem
    return f"{p.parent.name}_{p.stem}" if p.parent.name not in ("", ".") else p.stem


def _workers():
    try:
        n = len(os.sched_getaffinity(0))
    except AttributeError:
        n = os.cpu_count() or 1
    return max(1, min(n, 8))


def make_budget(deadline):
    """Returns stage(default_timeout) -> seconds to allow, shrunk to whatever is
    left before `deadline` (None = unlimited). Raises BudgetExhausted at zero."""
    def stage(default):
        if deadline is None:
            return default
        left = int(deadline - time.time())
        if left <= 0:
            raise BudgetExhausted()
        return min(default, left)
    return stage


def _kill_group(pid):
    try:
        os.killpg(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def run_group(cmd, timeout, env=None, capture=True):
    """Run cmd in its own process group; the whole group is killed on timeout,
    on Ctrl-C and after a normal exit, so encoder threads, Xvfb and BinkPlayer64
    never outlive the call. Re-raises subprocess.TimeoutExpired."""
    proc = subprocess.Popen(
        cmd, env=env, start_new_session=True, text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    try:
        out, err = proc.communicate(timeout=timeout)
    except BaseException:  # timeout, KeyboardInterrupt, ...
        _kill_group(proc.pid)
        try:
            proc.communicate()
        except Exception:  # noqa: BLE001
            pass
        raise
    _kill_group(proc.pid)
    return proc.returncode, out or "", err or ""


# --------------------------------------------------------------------------- #
# .bk2 header
# --------------------------------------------------------------------------- #
# Layout: 0 magic | 4 file size | 8 frames | 12 largest frame | 16 frames |
#         20 width | 24 height | 28 fps numerator | 32 fps denominator

def read_bk2_header(path):
    with open(path, "rb") as f:
        h = f.read(36)
    if len(h) < 36:
        raise ValueError("header_truncated")
    frames = struct.unpack_from("<I", h, 8)[0]
    width, height = struct.unpack_from("<2I", h, 20)
    fps_num, fps_den = struct.unpack_from("<2I", h, 28)
    return {"magic": h[:4], "frames": frames, "width": width, "height": height,
            "fps_num": fps_num, "fps_den": fps_den}


def meta_from_header(bk2_path):
    h = read_bk2_header(bk2_path)
    if h["frames"] <= 0 or h["width"] <= 0 or h["height"] <= 0:
        raise ValueError(f"header_zero_field_frames={h['frames']}_size={h['width']}x{h['height']}")
    fps = round(h["fps_num"] / h["fps_den"]) if h["fps_den"] else 30
    return {"width": h["width"], "height": h["height"],
            "fps": max(1, int(fps)), "frame_count": h["frames"]}


def meta_from_frames(frames_dir):
    """Meta for a directory of PNGs the agent made themselves."""
    frames_dir = Path(frames_dir)
    pngs = sorted(frames_dir.glob("*.png"))
    if not pngs:
        raise ValueError("no_png_frames")
    mp = frames_dir / "meta.json"
    if mp.exists():
        try:
            m = json.loads(mp.read_text())
            return {"width": m["width"], "height": m["height"],
                    "fps": m.get("fps", 30), "frame_count": len(pngs)}
        except (ValueError, KeyError, OSError):
            pass
    with Image.open(pngs[0]) as im:
        width, height = im.size
    return {"width": width, "height": height, "fps": 30, "frame_count": len(pngs)}


def check_agent_header(bk2_path, meta):
    """None if the agent's .bk2 header is acceptable, else a failure reason."""
    try:
        h = read_bk2_header(bk2_path)
    except ValueError as e:
        return str(e)
    if h["magic"] != REQUIRED_MAGIC:
        return f"invalid_magic_{h['magic']!r}_expected_{REQUIRED_MAGIC!r}"
    if (h["width"], h["height"]) != (meta["width"], meta["height"]):
        return (f"header_size_mismatch_{h['width']}x{h['height']}"
                f"_expected_{meta['width']}x{meta['height']}")
    if h["frames"] != meta["frame_count"]:
        return f"header_frame_count_mismatch_{h['frames']}_expected_{meta['frame_count']}"
    return None


# --------------------------------------------------------------------------- #
# Decoding and reference preparation
# --------------------------------------------------------------------------- #

def decode_bk2(wrapper, bk2_path, out_dir, timeout):
    """Returns (returncode, last output line). Raises subprocess.TimeoutExpired.
    The wrapper's own wait is set 20s below `timeout` so its cleanup trap runs
    before we would have to kill it."""
    env = {**os.environ, "BINK_DECODE_TIMEOUT": str(max(10, int(timeout) - 20))}
    rc, out, err = run_group(
        [str(wrapper), "--bk2", str(bk2_path), "--output", str(out_dir)],
        timeout, env=env)
    lines = [ln.strip() for ln in (out + "\n" + err).splitlines() if ln.strip()]
    return rc, (lines[-1] if lines else "")


def prepare_reference(ref_source, cid, wrapper, timeout_fn):
    """Returns (frames_dir, meta). A directory is used as-is; a .bk2 is decoded
    once into the cache (and re-used if the cached decode is complete and was
    made from the same file). `timeout_fn()` gives the seconds allowed for each
    attempt and may raise BudgetExhausted. Retries once: the player is a
    real-time GL app and can flake."""
    ref_source = Path(ref_source)
    if ref_source.is_dir():
        return ref_source, meta_from_frames(ref_source)

    meta = meta_from_header(ref_source)
    size = ref_source.stat().st_size
    cache_dir = REF_CACHE_DIR / cid

    mp = cache_dir / "meta.json"
    if mp.exists():
        try:
            cached = json.loads(mp.read_text())
            if (cached.get("source_size") == size
                    and cached.get("frame_count") == meta["frame_count"]
                    and len(list(cache_dir.glob("*.png"))) == meta["frame_count"]):
                return cache_dir, cached
        except (ValueError, OSError):
            pass

    last = "unknown"
    for _ in range(2):
        t = timeout_fn()
        shutil.rmtree(cache_dir, ignore_errors=True)
        cache_dir.mkdir(parents=True, exist_ok=True)
        try:
            rc, detail = decode_bk2(wrapper, ref_source, cache_dir, t)
        except subprocess.TimeoutExpired:
            last = f"decode_timeout_after_{t}s"
            continue
        n = len(list(cache_dir.glob("*.png")))
        if rc == 0 and n == meta["frame_count"]:
            for m in cache_dir.glob("*.meta"):
                m.unlink()
            meta = {**meta, "source": ref_source.name, "source_size": size}
            (cache_dir / "meta.json").write_text(json.dumps(meta, indent=2))
            return cache_dir, meta
        last = f"decode_rc{rc}_frames_{n}_of_{meta['frame_count']}: {detail}"
    raise ReferenceDecodeError(last)


# --------------------------------------------------------------------------- #
# Encoding
# --------------------------------------------------------------------------- #

def run_encoder(encoder, frames_dir, out_bk2, meta, timeout):
    """Run the agent's encoder. Returns None on success, else a failure reason."""
    out_bk2 = Path(out_bk2)
    out_bk2.unlink(missing_ok=True)  # never score a stale file from an earlier run
    cmd = [str(encoder), "--input", str(frames_dir), "--output", str(out_bk2),
           "--width", str(meta["width"]), "--height", str(meta["height"]),
           "--fps", str(meta["fps"])]
    try:
        rc, _, _ = run_group(cmd, timeout, capture=False)
    except subprocess.TimeoutExpired:
        return "encode_timeout"
    if rc != 0:
        return f"encoder_exit_{rc}"
    if not out_bk2.exists():
        return "no_output_file"
    return None


# --------------------------------------------------------------------------- #
# Quality scoring
# --------------------------------------------------------------------------- #

def _score_pair(paths):
    src_p, dec_p = paths
    with Image.open(src_p) as a, Image.open(dec_p) as b:
        s = np.asarray(a.convert("RGB"))
        d = np.asarray(b.convert("RGB"))
    if s.shape != d.shape:
        return ("shape", s.shape, d.shape)
    with np.errstate(divide="ignore"):
        psnr = float(sk_psnr(s, d, data_range=255))
    # Per-frame cap: PSNR is reported on a 0-50 scale, and a lossless frame
    # would otherwise be +inf (which is also not valid JSON).
    psnr = min(psnr, PSNR_CEILING)
    ssim = float(sk_ssim(s, d, channel_axis=2, data_range=255))
    return ("ok", psnr, ssim)


def compute_quality(src_dir, dec_dir):
    """PSNR/SSIM over all frame pairs, streamed (never holds a clip in RAM)."""
    src = sorted(Path(src_dir).glob("*.png"))
    dec = sorted(Path(dec_dir).glob("*.png"))
    if not src:
        raise ValueError("no_source_frames")
    if len(src) != len(dec):
        raise ValueError(f"frame_count_mismatch_source_{len(src)}_decoded_{len(dec)}")
    with ProcessPoolExecutor(max_workers=_workers()) as ex:
        res = list(ex.map(_score_pair, zip(src, dec), chunksize=4))
    for i, r in enumerate(res):
        if r[0] == "shape":
            raise ValueError(f"frame_{i}_shape_mismatch_{r[1]}_vs_{r[2]}")
    psnr = [r[1] for r in res]
    ssim = [r[2] for r in res]
    return {"psnr": float(np.mean(psnr)), "psnr_min": float(np.min(psnr)),
            "ssim": float(np.mean(ssim)), "frames_scored": len(res)}


def first_frame_index(frame_dir):
    """Lowest frame_NNNNNN index present (avoids misaligning the two streams if
    their numbering differs)."""
    indices = []
    for p in Path(frame_dir).glob("frame_*.png"):
        m = re.match(r"frame_(\d+)\.png$", p.name)
        if m:
            indices.append(int(m.group(1)))
    if not indices:
        raise ValueError(f"No frame_NNNNNN.png files found in {frame_dir}")
    return min(indices)


def compute_vmaf(src_dir, dec_dir, fps, timeout):
    """Returns (vmaf, None) on success or (None, error_text)."""
    log = Path(tempfile.mktemp(suffix=".json"))
    try:
        src_start = first_frame_index(src_dir)
        dec_start = first_frame_index(dec_dir)
        cmd = [
            FFMPEG, "-y",
            "-start_number", str(src_start), "-framerate", str(fps),
            "-i", str(Path(src_dir) / "frame_%06d.png"),
            "-start_number", str(dec_start), "-framerate", str(fps),
            "-i", str(Path(dec_dir) / "frame_%06d.png"),
            "-lavfi", f"{VMAF_FILTER}:n_threads={_workers()}:log_path={log}:log_fmt=json",
            "-f", "null", "-",
        ]
        subprocess.run(cmd, capture_output=True, timeout=timeout, check=True)
        data = json.loads(log.read_text())
        pooled = data.get("pooled_metrics", data.get("pooled", {}))
        block = pooled.get("vmaf", {})
        return float(block.get("mean", block.get("harmonic_mean"))), None
    except subprocess.CalledProcessError as e:
        tail = e.stderr.decode(errors="replace").strip().splitlines()[-1:] or [""]
        return None, f"ffmpeg_exit_{e.returncode}: {tail[0]}"
    except Exception as e:  # noqa: BLE001 - any failure means "no VMAF", reported loudly
        return None, f"{type(e).__name__}: {e}"
    finally:
        log.unlink(missing_ok=True)


def geomean(psnr, ssim, vmaf):
    psnr_norm = min(psnr / PSNR_CEILING, 1.0)
    if vmaf is None:
        return (psnr_norm * ssim) ** 0.5
    return (psnr_norm * ssim * (vmaf / 100.0)) ** (1.0 / 3.0)


def composite_reward(quality_geomean, bpp, successful, total):
    """Quality discounted by efficiency, then by completion rate (so a lucky,
    efficient subset of clips cannot earn an inflated reward).
    BPP_REFERENCE is a placeholder anchor, not yet calibrated."""
    if bpp is None or bpp <= 0 or total == 0:
        return 0.0
    efficiency = min(BPP_REFERENCE / bpp, 1.0)
    completion = successful / total
    return quality_geomean * efficiency * completion


# --------------------------------------------------------------------------- #
# One clip, end to end
# --------------------------------------------------------------------------- #

def _evaluate(ref_source, cid, encoder, wrapper, limits, stage, verifier, out_bk2):
    fail = {"clip": cid, "decode_failed": True}

    if not Path(encoder).exists():
        return {**fail, "reason": f"no_encoder_at_{encoder}"}

    # Reference: decoded once; the SAME frames are the encoder input and the
    # scoring reference. A reference failure is the harness's fault, not the
    # agent's, so in verifier mode it is flagged infra_error (and cannot be
    # caused by the agent: it happens before their code ever runs).
    try:
        ref_dir, meta = prepare_reference(
            ref_source, cid, wrapper, lambda: stage(limits["ref"]))
    except (ReferenceDecodeError, ValueError) as e:
        out = {**fail, "reason": f"reference_prep_failed: {e}"}
        if verifier:
            out["infra_error"] = True
        return out

    reason = run_encoder(encoder, ref_dir, out_bk2, meta, stage(limits["encode"]))
    if reason:
        return {**fail, "reason": reason}

    reason = check_agent_header(out_bk2, meta)
    if reason:
        return {**fail, "reason": reason}

    with tempfile.TemporaryDirectory(prefix="binkbench_dec_") as tmp:
        dec_dir = Path(tmp)
        try:
            rc, detail = decode_bk2(wrapper, out_bk2, dec_dir, stage(limits["decode"]))
        except subprocess.TimeoutExpired:
            return {**fail, "reason": "decode_timeout"}
        if rc != 0:
            return {**fail, "reason": f"decode_failed_rc{rc}", "detail": detail}

        try:
            quality = compute_quality(ref_dir, dec_dir)
        except ValueError as e:
            return {**fail, "reason": str(e)}

        vmaf, vmaf_err = compute_vmaf(ref_dir, dec_dir, meta["fps"], stage(limits["vmaf"]))

    n = quality["frames_scored"]
    w, h = meta["width"], meta["height"]
    size = out_bk2.stat().st_size
    result = {
        "clip": cid,
        "decode_failed": False,
        "frame_count": n,
        # Denominator is raw 8-bit RGB size: stable, and independent of how the
        # player/hooker happened to compress the reference PNGs.
        "compression_ratio": round(size / (w * h * 3 * n), 5),
        "bpp": round(size * 8 / (w * h * n), 5),
        "psnr": round(quality["psnr"], 3),
        "psnr_min": round(quality["psnr_min"], 3),
        "ssim": round(quality["ssim"], 4),
        "vmaf": round(vmaf, 3) if vmaf is not None else None,
        "quality_geomean": round(geomean(quality["psnr"], quality["ssim"], vmaf), 4),
    }
    if vmaf is None:
        result["vmaf_missing"] = True
        result["vmaf_error"] = vmaf_err
        if verifier:
            # Without VMAF the score is on a different scale: environment
            # problem, not an agent failure. Excluded and flagged, never silent.
            result.update({"decode_failed": True, "infra_error": True,
                           "reason": "vmaf_unavailable"})
    return result


def evaluate_clip(ref_source, encoder, wrapper, limits, deadline=None,
                  verifier=False, root=None):
    """Always returns a result dict. Never raises (except KeyboardInterrupt)."""
    cid = clip_id(ref_source, root)
    out_bk2 = Path(f"/tmp/output_{cid}.bk2")
    stage = make_budget(deadline)
    try:
        return _evaluate(ref_source, cid, encoder, wrapper, limits, stage, verifier, out_bk2)
    except BudgetExhausted:
        return {"clip": cid, "decode_failed": True, "reason": "verifier_budget_exhausted"}
    except Exception as e:  # noqa: BLE001
        # Counted as a failure (not excluded): excluding unexplained errors would
        # let a clip that breaks the scorer escape a zero.
        return {"clip": cid, "decode_failed": True, "harness_exception": True,
                "reason": f"scorer_exception_{type(e).__name__}: {e}"}
    finally:
        if verifier:  # keep disk flat across clips; agents keep their output
            out_bk2.unlink(missing_ok=True)
            shutil.rmtree(REF_CACHE_DIR / cid, ignore_errors=True)


# --------------------------------------------------------------------------- #
# Verifier batch mode
# --------------------------------------------------------------------------- #

def _write_reward(report):
    """Harbor requires reward.json to be a flat object of NUMERIC metrics, so it
    gets only the numbers (booleans become 0/1, nulls are dropped). The full
    report, with per-clip results and any error text, goes to report.json next
    to it; /logs is downloaded to the host after grading."""
    REWARD_PATH.parent.mkdir(parents=True, exist_ok=True)
    numeric = {k: (int(v) if isinstance(v, bool) else v)
               for k, v in report.items()
               if isinstance(v, (int, float)) and v is not None}
    numeric.setdefault("reward", 0.0)
    REWARD_PATH.write_text(json.dumps(numeric, indent=2))
    (REWARD_PATH.parent / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


def run_verifier_batch(held_out_path, encoder, wrapper):
    start = time.time()
    deadline = start + TOTAL_VERIFIER_TIMEOUT - SHUTDOWN_BUFFER
    held_out_path = Path(held_out_path)

    if not held_out_path.is_dir():
        _write_reward({"reward": 0.0, "error": f"held_out_dir_missing_{held_out_path}", "clips": []})
        return 1
    clips = sorted(held_out_path.rglob("*.bk2")) or \
        sorted(d for d in held_out_path.iterdir() if d.is_dir())
    if not clips:
        _write_reward({"reward": 0.0, "error": f"no_clips_found_in_{held_out_path}", "clips": []})
        return 1

    results = []
    for clip in clips:
        print(f"[evaluation] scoring {clip.name} ...", file=sys.stderr)
        results.append(evaluate_clip(clip, encoder, wrapper, VERIFIER_LIMITS,
                                     deadline=deadline, verifier=True, root=held_out_path))

    infra = [r for r in results if r.get("infra_error")]
    graded = [r for r in results if not r.get("infra_error")]
    ok = [r for r in graded if not r.get("decode_failed", True)]

    # Failed clips score as zero (not excluded); only infrastructure errors are
    # excluded, and they are always reported.
    geomeans = [r["quality_geomean"] if not r.get("decode_failed", True) else 0.0 for r in graded]
    mean_quality = float(np.mean(geomeans)) if geomeans else 0.0
    mean_bpp = float(np.mean([r["bpp"] for r in ok])) if ok else None
    mean_cr = float(np.mean([r["compression_ratio"] for r in ok])) if ok else None

    reward = composite_reward(mean_quality, mean_bpp, len(ok), len(graded))

    report = {
        "reward": round(reward, 4),
        "valid": bool(graded) and not infra,
        "mean_quality_geomean": round(mean_quality, 4),
        "mean_bpp": round(mean_bpp, 5) if mean_bpp is not None else None,
        "mean_compression_ratio": round(mean_cr, 5) if mean_cr is not None else None,
        "clips_total": len(results),
        "clips_graded": len(graded),
        "clips_failed": len(graded) - len(ok),
        "clips_infra_error": len(infra),
        "clips": results,
    }
    if infra:
        report["warning"] = (f"{len(infra)} clip(s) hit infrastructure errors and were excluded "
                             f"from the reward; re-run recommended")
    if not graded:
        report["error"] = "no_gradable_clips"
    _write_reward(report)
    return 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser(description="BinkBench evaluation and scoring")
    ap.add_argument("--clip", help="reference .bk2 (e.g. /app/samples/cgi/Sample_A.bk2) "
                                   "or a directory of your own PNG frames")
    ap.add_argument("--frames", help="alias for --clip")
    ap.add_argument("--decode-only", action="store_true",
                    help="with --clip: decode the reference to PNG frames, print the "
                         "directory and exit (so you can run your encoder on it by hand)")
    ap.add_argument("--held-out", help="held-out clips directory (verifier mode)")
    ap.add_argument("--encoder", default=DEFAULT_ENCODER)
    ap.add_argument("--wrapper", default=DEFAULT_WRAPPER)
    args = ap.parse_args()

    target = args.clip or args.frames
    if target:
        target = Path(target)
        if not target.exists():
            print(json.dumps({"error": f"not_found_{target}"}))
            sys.exit(1)

        if args.decode_only:
            cid = clip_id(target)
            try:
                ref_dir, meta = prepare_reference(
                    target, cid, args.wrapper, lambda: AGENT_LIMITS["ref"])
            except (ReferenceDecodeError, ValueError) as e:
                print(json.dumps({"clip": cid, "decode_failed": True,
                                  "reason": f"reference_prep_failed: {e}"}, indent=2))
                sys.exit(1)
            print(json.dumps({"clip": cid, "frames_dir": str(ref_dir), **meta}, indent=2))
            sys.exit(0)

        res = evaluate_clip(target, args.encoder, args.wrapper, AGENT_LIMITS)
        print(json.dumps(res, indent=2))
        if res.get("detail"):
            print(f"[evaluation] decoder said: {res['detail']}", file=sys.stderr)
        if res.get("vmaf_missing"):
            print(f"[evaluation] WARNING: VMAF unavailable ({res.get('vmaf_error')}); "
                  f"quality_geomean excludes VMAF and is NOT comparable to grading.",
                  file=sys.stderr)
        sys.exit(0 if not res.get("decode_failed", True) else 1)

    held_out = args.held_out or (str(HELD_OUT_DIR) if HELD_OUT_DIR.exists() else None)
    if held_out:
        sys.exit(run_verifier_batch(held_out, args.encoder, args.wrapper))

    ap.print_help()
    sys.exit(1)


if __name__ == "__main__":
    main()
