FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Build toolchains + runtime dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    python3 \
    python3-pip \
    rustc \
    cargo \
    golang-go \
    git \
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

# Run for ffmpeg with libvmaf
RUN wget -q https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz -O /tmp/ffmpeg.tar.xz && \
    mkdir -p /tmp/ffmpeg-extract && \
    tar xf /tmp/ffmpeg.tar.xz -C /tmp/ffmpeg-extract --strip-components=1 && \
    mv /tmp/ffmpeg-extract/ffmpeg /usr/local/bin/ffmpeg && \
    mv /tmp/ffmpeg-extract/ffprobe /usr/local/bin/ffprobe && \
    rm -rf /tmp/ffmpeg.tar.xz /tmp/ffmpeg-extract


# Benchmark scoring and self_eval dependencies
RUN pip3 install --no-cache-dir \
    numpy \
    opencv-python-headless \
    scikit-image \
    Pillow \
    "huggingface_hub[cli]"


# Create runtime directories
RUN mkdir -p \
    /app/internal \
    /app/tools \
    /app/nihav \
    /app/samples


# Agent-visible resources

# Decoder reference implementation (source only)
COPY nihav/ /app/nihav/

# Download sample material available for iteration
RUN wget -q https://huggingface.co/datasets/MaskNinja/BinkBenchAssets/resolve/main/samples.tar.gz -O /tmp/samples.tar.gz && \
    tar xzf /tmp/samples.tar.gz -C /app/ && \
    rm /tmp/samples.tar.gz


# Internal oracle
# Agents interact only through decode_wrapper.sh/self_eval.py

COPY tools/BinkPlayer64 /app/internal/BinkPlayer64
COPY tools/bink_hooker.so /app/internal/bink_hooker.so

RUN chmod +x /app/internal/BinkPlayer64


# Benchmark interface

COPY tools/self_eval.py /app/tools/self_eval.py
COPY tools/decode_wrapper.sh /app/tools/decode_wrapper.sh

RUN chmod +x \
    /app/tools/self_eval.py \
    /app/tools/decode_wrapper.sh


# Runtime paths

ENV BINKHOOKER_SO="/app/internal/bink_hooker.so"
ENV DECODE_WRAPPER="/app/tools/decode_wrapper.sh"

ENV LD_LIBRARY_PATH=/app/internal

ENV PATH="/app/tools:$PATH"

# For the decode wrapper
ENV BINKPLAYER_PATH="/app/internal/BinkPlayer64"

# Agent output + logs

RUN mkdir -p \
    /output \
    /logs/agent \
    /logs/verifier


WORKDIR /app


# Default interactive shell
# Benchmark runners can override this command
CMD ["/bin/bash"]