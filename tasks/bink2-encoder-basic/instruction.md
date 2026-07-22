# Building a Bink 2 Encoder

Your task is to create a Bink 2 encoder from scratch, using the resources and tools available in the repo, within one week.

## Requirements

Your encoder must:

- be able to understand the bitstream format that Bink 2's decoder uses
- be able to support a number of different scenes and support scene changes / keyframe triggering
- be able to support gradual scene changes
- support inter-frame prediction (not solely intra-frame / all-keyframe encoding)
- be able to support the output of Bink 2 videos in 1080p30
- implement any heuristics or optimisation strategies you consider appropriate

Produce the highest-quality and most efficient encoder you can within the allotted time. How you implement encoder heuristics and compression logic is entirely up to you, as long as it follows the Bink 2 bitstream syntax. You may install any language if the languages currently installed do not suit you.

Do **not** search for reference Bink 2 encoders online; you will not find them or will find a version incompatible with the decoder you are being tested on. However, you are free to search for information regarding video compression in general.

## Interface

Your final executable must be located at, and have the exact syntax of:

```
/output/encoder --input <frames dir> --output <video-name.bk2> --width <width> --height <height> --fps <fps>
```

Example:

```
/output/encoder --input /app/samples/real_scene --output real_scene.bk2 --width 1920 --height 1080 --fps 30
```

## Grading

Your submission will be evaluated using the following objective metrics:

**Quality** measured by:

- **VMAF** (as primary)
- **PSNR**, on a scale of 0–50
- **SSIM**
- a **geometric mean** of the normalized VMAF, PSNR, and SSIM scores

**Efficiency** measured by:

- **compression ratio**
- **bits per pixel (bpp)**

## Time constraints

At grading time, your submitted `/output/encoder` will be run against
held-out clips under a strict total time budget for the entire
evaluation. An encoder that produces excellent quality but takes too
long per clip risks not completing in time — in which case that clip
will score as a failure, regardless of the quality it might have
eventually achieved.

Design for a reasonable balance of speed and quality, not exhaustive
search for the theoretical best possible encode. Consider whether
your approach can take advantage of multiple CPU cores if it helps
you finish within the time budget.

## Tools

### `self_eval.py`

To encode and receive stats on a clip, run:

```
self_eval.py --frames <frames dir>
```

`self_eval.py` uses your encoder located at `/output/encoder` to encode the clip, and writes the resulting `.bk2` to `/tmp/output_<your folder name>.bk2`.

`self_eval.py` runs your encoder, decodes the resulting `.bk2`, compares the decoded frames against the originals, and reports the same metrics used for grading, so you can iterate before submitting.

### `decode_wrapper.sh`

To decode a video, run:

```
decode_wrapper.sh --bk2 <bk2 file or dir> --output <output frames dir>
```

Use `decode_wrapper.sh` directly if you want to inspect a specific `.bk2` file's decoded frames in PNG format without running the full scoring pipeline.

## Samples

`/app/samples/` contains a set of clips (as folders of PNG frames) for you to test your encoder against during development.

These clips vary in motion, content type, and scene structure — some contain hard scene cuts, and you may encounter gradual transitions (crossfades) as well. They are representative of, but not identical, to the clips used for final grading. You are permitted to resize or modify the images if it helps with encoder development, or create your own frame samples using scripts, but keep in mind the final videos you are graded on are similar to your samples, so you should test on them.

`/app/samples/` contains two categories of clips (`cgi_scene*` and `real_scene*`), each folder numbered in what I judge to be increasing order of complexity within its own category (e.g. `real_scene` is intended to be simpler than `real_scene_2`, which is simpler than `real_scene_3`). Complexity is not necessarily comparable across categories.

## NihAV

You have access to NihAV, specifically access to a reimplementation of the Bink 2 decoder. Note that a Bink 1 encoder is  present, but that may not be relavant to most portions of your work.

## Notes

If your video does not decode by either `self_eval.py` or `decode_wrapper.sh`, they will note it.
Your final submission will be evaluated on held-out clips that are not included in the repository.