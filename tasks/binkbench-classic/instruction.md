# Building a Bink 2 Encoder

Your task is to create a Bink 2 encoder from scratch, using the resources and tools available in the repo, within one week.

## Requirements

Your encoder must:

- be able to understand the bitstream format that Bink 2's decoder uses
- produce output in the **KB2i** bitstream revision (your file's first four bytes must be exactly `KB2i`) — submissions in any other format, including other Bink 2 revisions or Bink 1, are rejected before decoding and score zero
- write the true display resolution and frame count into the `.bk2` header — a header that disagrees with the input frames is rejected before decoding
- be able to support a number of different scenes and support scene changes / keyframe triggering
- be able to support gradual scene changes
- support inter-frame prediction (not solely intra-frame / all-keyframe encoding)
- be able to support the output of Bink 2 videos in 1080p30
- implement any heuristics or optimisation strategies you consider appropriate

You *do not* need to implement audio support.

Produce the highest-quality and most efficient encoder you can within the allotted time. How you implement encoder heuristics and compression logic is entirely up to you, as long as it follows the Bink 2 bitstream syntax. You may install any language if the languages currently installed do not suit you (but see **Where your encoder runs** below).

Do **not** search for reference Bink 2 encoders online; you will not find them or will find a version incompatible with the decoder you are being tested on. However, you are free to search for information regarding video compression in general.

## Interface

Your final executable must be located at, and have the exact syntax of:

```
/output/encoder --input <frames dir> --output <video-name.bk2> --width <width> --height <height> --fps <fps>
```

`<frames dir>` contains the clip as PNG frames named `frame_000000.png`, `frame_000001.png`, ... (8-bit RGB, numbered from 0). The reference videos are `.bk2` files: the grader decodes each one to these PNG frames once, runs your encoder on them, and scores your output against those same frames.

To get that frames directory for a sample (and the width, height and fps to pass), run:

```
evaluation.py --clip /app/samples/cgi/Sample_A.bk2 --decode-only
```

Example (using the directory it prints):

```
/output/encoder --input /tmp/binkbench_ref_cache/cgi_Sample_A --output test_video.bk2 --width 1920 --height 1080 --fps 30
```

## Where your encoder runs

Your artefacts required to run with your encoder **must all** be present in `/output`.

Your encoder is built and tested in this container but **graded in a different one**. Only the contents of `/output` are carried across — the encoder plus any config, data files or libraries it needs. The grading container has:

- Ubuntu 22.04 with Python 3 (numpy, scikit-image, Pillow), ffmpeg, and the standard C libraries
- **4 CPU cores and 4 GB of RAM** (this container has 8 cores and 8 GB)
- no compilers, no package installs, and no network

Anything else your encoder depends on — an interpreter or runtime you installed, extra Python packages, shared libraries, table or model files — must be inside `/output` (or statically linked), and the encoder must locate it relative to its own path. Keep `/output` small (under 2 GB). An encoder that runs here but cannot start in the grading container fails every clip.

## Grading

For each held-out clip your encoder is run, its output is decoded with the same decoder you have here, and the decoded frames are compared with the reference frames. Your output is rejected (the clip scores zero) if the header is not `KB2i`, declares the wrong resolution or frame count, fails to decode, or decodes to a different number of frames.

**Quality** is measured by:

- **VMAF** (as primary)
- **PSNR**, on a scale of 0–50 (each frame is capped at 50 dB)
- **SSIM**
- a **geometric mean** of the normalized VMAF, PSNR, and SSIM scores

**Efficiency** is measured by:

- **compression ratio** (encoded size divided by raw 8-bit RGB size)
- **bits per pixel (bpp)**

## Time constraints

At grading time, your submitted `/output/encoder` is run on each held-out clip in turn (several clips), under one shared time budget of roughly three hours for the whole evaluation. That budget also pays for decoding the references, decoding your output, and computing the metrics. A single encode is hard-capped at 30 minutes, but because the budget is shared you should **plan for about 15 minutes per clip on average**. A clip that runs out of budget, or exceeds its cap, scores as a failure regardless of the quality it might have eventually achieved.

Design for a reasonable balance of speed and quality, not exhaustive search for the theoretical best possible encode. The grading container has 4 cores, so consider whether your approach can take advantage of multiple CPU cores.

## Tools

### `evaluation.py`

To encode a sample and receive stats on it, run:

```
evaluation.py --clip <reference .bk2>
```

For example `evaluation.py --clip /app/samples/cgi/Sample_A.bk2`. It decodes the reference to PNG frames (cached under `/tmp/binkbench_ref_cache/`), runs your encoder at `/output/encoder` on them, writes your output to `/tmp/output_<folder>_<name>.bk2` (for example `/tmp/output_cgi_Sample_A.bk2`), decodes it, and reports the same metrics used for grading as JSON, so you can iterate before submitting. It stops an encode after 15 minutes.

Other forms:

```
evaluation.py --clip <reference .bk2> --decode-only   # only extract the reference frames and print their directory
evaluation.py --clip <directory of PNG frames>        # score material you made yourself
```

A directory of your own frames should use the `frame_000000.png` naming. Put a `meta.json` with `width`, `height` and `fps` in it, otherwise 30 fps is assumed and the size is read from the first frame.

If a result has `"decode_failed": true`, the `reason` field says why (for example `header_size_mismatch`, `invalid_magic` or `frame_count_mismatch`).

### `decode_wrapper.sh`

To decode a video, run:

```
decode_wrapper.sh --bk2 <bk2 file> --output <output frames dir>
```

Use `decode_wrapper.sh` directly if you want to inspect a specific `.bk2` file's decoded frames in PNG format without running the full scoring pipeline. The output size and frame count come from the file's header, exactly as at grading.

## Samples

`/app/samples/` contains genuine KB2i `.bk2` videos for you to study and test your encoder against, in two categories: `cgi/` and `real/`. Within each category, clips are lettered (`Sample_A`, `Sample_B`, ...) in increasing order of complexity. Complexity, and scores, are not comparable across categories or clips.

These clips vary in motion, content type, and scene structure — some contain hard scene cuts, and you may encounter gradual transitions (crossfades) as well. They are representative of, but not identical to, the clips used for final grading. You can read them with NihAV, decode them with `decode_wrapper.sh`, and use them as targets. You are permitted to trim, resize or modify the videos if it helps with encoder development, or create your own frame samples using scripts, but keep in mind the final videos you are graded on are similar to your samples, so you should test on them.

## NihAV

You have access to NihAV, specifically access to a reimplementation of the Bink 2 decoder. Note that a Bink 1 encoder is present, but that may not be relevant to most portions of your work.

## Notes

If your video does not decode by either `evaluation.py` or `decode_wrapper.sh`, they will note it. Your final submission will be evaluated on held-out clips that are not included in the repository.
