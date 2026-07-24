FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive

# ... same apt-get install, ffmpeg static build, pip install steps as the agent Dockerfile ...

WORKDIR /app
RUN mkdir -p /app/internal /tests

COPY tools/BinkPlayer64 /app/internal/BinkPlayer64
COPY tools/bink_hooker.so /app/internal/bink_hooker.so
RUN chmod +x /app/internal/BinkPlayer64

COPY tools/decode_wrapper.sh /app/tools/decode_wrapper.sh
RUN chmod +x /app/tools/decode_wrapper.sh

COPY tests/metrics.py /tests/metrics.py
COPY tests/test.sh /tests/test.sh
RUN chmod +x /tests/test.sh

RUN wget -q https://huggingface.co/datasets/MaskNinja/BinkBenchAssets/resolve/main/held-out.tar.gz -O /tmp/held-out.tar.gz && \
    tar xzf /tmp/held-out.tar.gz -C /tests/ && \
    rm /tmp/held-out.tar.gz

ENV LD_LIBRARY_PATH=/app/internal
ENV BINKPLAYER_PATH=/app/internal/BinkPlayer64
ENV BINKHOOKER_SO=/app/internal/bink_hooker.so
ENV DECODE_WRAPPER=/app/tools/decode_wrapper.sh

CMD ["/tests/test.sh"]