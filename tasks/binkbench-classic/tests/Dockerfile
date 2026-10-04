# Verifier image for BinkBench.
#
# This is the environment in which /output/encoder (built by the agent in the
# agent image) is graded against the held-out clips. Its decode + scoring
# stack MUST mirror tasks/binkbench-classic/environment/Dockerfile exactly —
# the same Xvfb/OpenGL/X11 libs, the same libvmaf-enabled ffmpeg, the same
# Python scoring deps — so that what an agent saw during evaluation.py is what it
# gets at grading. It deliberately omits everything agent-side: no build
# toolchain (rust/cargo/golang/cmake), no NihAV source, no samples, no
# /app/nihav. The held-out clips live here and only here.

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

COPY evaluation.py /tests/evaluation.py
COPY test.sh /tests/test.sh

# Windows checkouts can leave CRLF line endings; a CRLF shebang cannot execute on Linux.
RUN sed -i 's/\r$//' /app/tools/decode_wrapper.sh /tests/test.sh /tests/evaluation.py && \
    chmod +x /tests/test.sh

# Held-out clips: genuine KB2i .bk2 files baked into the image, so grading is
# hermetic (nothing is downloaded). tests/held-out/ is PRIVATE: keep it out of the
# public repo (gitignored). Build context is tests/ itself.
COPY held-out/ /tests/held-out/
RUN set -e; \
    n=$(find /tests/held-out -name '*.bk2' | wc -l); \
    [ "$n" -gt 0 ] || { echo "no .bk2 files in /tests/held-out"; exit 1; }; \
    for f in $(find /tests/held-out -name '*.bk2'); do \
      [ "$(head -c4 "$f")" = "KB2i" ] || { echo "non-KB2i held-out clip: $f"; exit 1; }; \
    done

ENV LD_LIBRARY_PATH=/app/internal
ENV BINKPLAYER_PATH=/app/internal/BinkPlayer64
ENV BINKHOOKER_SO=/app/internal/bink_hooker.so
ENV DECODE_WRAPPER=/app/tools/decode_wrapper.sh
ENV PATH=/app/tools:$PATH
ENV HELD_OUT_DIR=/tests/held-out
ENV REWARD_PATH=/logs/verifier/reward.json

CMD ["/tests/test.sh"]