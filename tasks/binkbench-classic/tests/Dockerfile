# Verifier image for BinkBench.
#
# This is the environment in which /output/encoder (built by the agent in the
# agent image) is graded against the held-out clips. Its decode + scoring
# stack MUST mirror tasks/bink2-encoder-basic/environment/Dockerfile exactly —
# the same Xvfb/OpenGL/X11 libs, the same libvmaf-enabled ffmpeg, the same
# Python scoring deps — so that what an agent saw during self_eval is what it
# gets at grading. It deliberately omits everything agent-side: no build
# toolchain (rust/cargo/golang/cmake), no NihAV source, no samples, no
# self_eval.py, no /app/nihav. The held-out clips live here and only here.

FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    xvfb \
    libx11-6 \
    libxext6 \
    libgl1 \
    libglx0 \
    libglib2.0-0 \
    zlib1g \
    wget \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN wget -q https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz -O /tmp/ffmpeg.tar.xz && \
    mkdir -p /tmp/ffmpeg-extract && \
    tar xf /tmp/ffmpeg.tar.xz -C /tmp/ffmpeg-extract --strip-components=1 && \
    mv /tmp/ffmpeg-extract/ffmpeg /usr/local/bin/ffmpeg && \
    mv /tmp/ffmpeg-extract/ffprobe /usr/local/bin/ffprobe && \
    rm -rf /tmp/ffmpeg.tar.xz /tmp/ffmpeg-extract

RUN pip3 install --no-cache-dir \
    numpy \
    scikit-image \
    Pillow

WORKDIR /app
RUN mkdir -p /app/internal /app/tools /tests /output /logs/agent /logs/verifier

# Internal oracle — build context is tests/ itself, binaries were copied into
# tests/tools/ specifically so this Dockerfile is self-contained regardless of
# which directory Harbor actually uses as the build context.
COPY tools/BinkPlayer64 /app/internal/BinkPlayer64
COPY tools/bink_hooker.so /app/internal/bink_hooker.so
RUN chmod +x /app/internal/BinkPlayer64

COPY tools/decode_wrapper.sh /app/tools/decode_wrapper.sh
RUN chmod +x /app/tools/decode_wrapper.sh

COPY metrics.py /tests/metrics.py
COPY test.sh /tests/test.sh
RUN chmod +x /tests/test.sh

RUN wget -q https://huggingface.co/datasets/MaskNinja/BinkBenchAssets/resolve/main/held-out.tar.gz -O /tmp/held-out.tar.gz && \
    tar xzf /tmp/held-out.tar.gz -C /tests/ && \
    rm /tmp/held-out.tar.gz

ENV LD_LIBRARY_PATH=/app/internal
ENV BINKPLAYER_PATH=/app/internal/BinkPlayer64
ENV BINKHOOKER_SO=/app/internal/bink_hooker.so
ENV DECODE_WRAPPER=/app/tools/decode_wrapper.sh
ENV PATH=/app/tools:$PATH
ENV HELD_OUT_DIR=/tests/held-out
ENV REWARD_PATH=/logs/verifier/reward.json

CMD ["/tests/test.sh"]