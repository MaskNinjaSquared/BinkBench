# BinkBench - a continuous, long-horizon benchmark for ambitious agents

*How good are agents at video engineering?*

## Intro

This benchmark is based off an idea which originated from [my Blogspot article](https://maskninjasquared.blogspot.com/2026/06/binkbench-can-we-compare-llm.html), although it's more centred on codec engineering, rather than reverse-engineering.

BinkBench is a proof-of-concept benchmark for coding agents based on four open metrics - VMAF (as primary), PSNR and SSIM for measuring video quality, as well as bits per pixel (bpp) for measuring efficiency. The former three metrics can also be comprised into one quality metric (via a geometric mean).

BinkBench is also contamination-resistant, since there are no public Bink 2 encoders available for agents to find. The agent must derive one from the decoder alone, not retrieve a known solution. This also allows us to have internet access on for any research into compression techniques for encoding, because no matter how hard they try, they won't be able to find a reference encoder.

## What's different about it?

The majority of popular benchmarks today score based on the amount of tasks completed, instead of how good you can get at tasks specifically - collapsing the ability to see how well an agent did on something specific to a score of zero or one. For example, simplified:

- **DeepSWE** - does the agent pass or fail this specific test?
- **FrontierCode** - should this be merged?

While this is a stable standard, it doesn't mean that every other benchmark should work this way. For example, [Vending-Bench](https://andonlabs.com/evals/vending-bench-2) was created in a manner similar to BinkBench.

## What are the agents supposed to do?

The agents are set up in an environment where they have access to NihAV and tools to call RAD's Bink 2 decoder for Linux. They're tasked with building an encoder from scratch to satisfy the decoder, focusing on getting as efficient and standard a video output as possible.

## Running BinkBench

BinkBench requires [Pier](https://github.com/datacurve-ai/pier). To run it:

```bash
git clone https://github.com/MaskNinjaSquared/BinkBench
uv tool install datacurve-pier

export API_KEY=...
pier run -p BinkBench/tasks/bink2-encoder-basic --agent mini-swe-agent --model provider/model
```

## Results

Agent scores will *in future* be plotted on a graph of VMAF against bits per pixel. Currently, scores are passed as a reward (made up of the geometric mean × efficiency) which also factors in completion of available validation samples. 

To run this at scale against multiple SOTA models, this requires an inference budget I don't have yet. However, if you're interested in running BinkBench against a model and sharing results, or want to help fund a broader run, feel free to reach out to me.

## Credits

- [HikingFex](https://www.hikingfex.com/) for sample videos used in both agent testing and external verification of the encoders
- [Playground Games](https://web.archive.org/web/20250122182716if_/https://playground-games.com/projects/forza-horizon-2/) for their cutscenes
- [NihAV](https://nihav.org/) for their implementation of the Bink 2 decoder
- [RAD Game Tools](https://www.radgametools.com/bnkmain.htm) for creating Bink 2!

## Notes

Please note that BinkBench is still a proof-of-concept, and I haven't been able to test this with models yet. I've tried to make the scripts as robust as possible, but if you find anything, reporting it would be a huge help.

## License

BinkBench is licensed under the [Apache 2.0 License](https://www.apache.org/licenses/LICENSE-2.0.html).
