# Agent image for BinkBench

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

# Copy and extract local pinned ffmpeg 7.0.2 static build
COPY tools/ffmpeg-7.0.2-amd64-static.tar.xz /tmp/ffmpeg.tar.xz
RUN mkdir -p /tmp/ffmpeg-extract && \
    tar xf /tmp/ffmpeg.tar.xz -C /tmp/ffmpeg-extract --strip-components=1 && \
    mv /tmp/ffmpeg-extract/ffmpeg /usr/local/bin/ffmpeg && \
    mv /tmp/ffmpeg-extract/ffprobe /usr/local/bin/ffprobe && \
    rm -rf /tmp/ffmpeg.tar.xz /tmp/ffmpeg-extract

# Fail the build now if this ffmpeg has no working libvmaf (the evaluation.py
# filter string, run end to end), instead of silently mis-scoring later.
RUN ffmpeg -hide_banner -loglevel error \
      -f lavfi -i testsrc=d=1:s=320x240:r=30 \
      -f lavfi -i testsrc=d=1:s=320x240:r=30 \
      -lavfi "libvmaf=model=version=vmaf_v0.6.1:n_threads=2:log_path=/tmp/vmaf_check.json:log_fmt=json" \
      -f null - && \
    grep -q '"pooled_metrics"' /tmp/vmaf_check.json && \
    rm -f /tmp/vmaf_check.json


# Benchmark scoring (evaluation.py) dependencies
RUN pip3 install --no-cache-dir \
    numpy \
    opencv-python-headless \
    scikit-image \
    Pillow


# Create runtime directories
RUN mkdir -p \
    /app/internal \
    /app/tools \
    /app/nihav \
    /app/samples


# Agent-visible resources

# Decoder reference implementation (source only)
COPY nihav/ /app/nihav/

# Sample material for iteration: genuine KB2i .bk2 files (cgi/, real/).
# evaluation.py decodes them to PNG frames on demand with decode_wrapper.sh.
COPY samples/bk2_samples/ /app/samples/

# Fail the build if any sample is not KB2i (a KB2g/KB2n file decodes differently)
RUN for f in $(find /app/samples -name '*.bk2'); do \
      [ "$(head -c4 "$f")" = "KB2i" ] || { echo "non-KB2i sample: $f"; exit 1; }; \
    done


# Internal oracle
# Agents interact only through decode_wrapper.sh/evaluation.py

COPY tools/BinkPlayer64 /app/internal/BinkPlayer64
COPY tools/bink_hooker.so /app/internal/bink_hooker.so

RUN chmod +x /app/internal/BinkPlayer64


# Benchmark interface

COPY tools/evaluation.py /app/tools/evaluation.py
COPY tools/decode_wrapper.sh /app/tools/decode_wrapper.sh

# Windows checkouts can leave CRLF line endings; a CRLF shebang cannot execute on Linux.
RUN sed -i 's/\r$//' /app/tools/decode_wrapper.sh /app/tools/evaluation.py

RUN chmod +x \
    /app/tools/evaluation.py \
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