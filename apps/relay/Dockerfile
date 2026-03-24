FROM ubuntu:22.04 AS builder
RUN apt-get update && apt-get install -y g++ libsqlite3-dev libcurl4-openssl-dev libssl-dev \
    libopus-dev pkg-config && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY main.cpp .
RUN g++ -O2 -std=c++17 -Wall -o soluna-relay main.cpp -lpthread -lsqlite3 -lcurl -lssl -lcrypto

# Install optional codec libraries (FLAC, fdk-aac) — build proceeds without them
COPY koecodec_enc.h codebooks_music.h .
COPY radio.cpp .
RUN apt-get update && \
    apt-get install -y --no-install-recommends libflac-dev libfdk-aac-dev 2>/dev/null; \
    rm -rf /var/lib/apt/lists/*

# Build radio with whichever codec libraries are available
RUN set -e; \
    RADIO_CFLAGS=""; RADIO_LIBS="-lpthread -lopus"; \
    if pkg-config --exists flac 2>/dev/null; then \
        RADIO_CFLAGS="$RADIO_CFLAGS -DHAVE_FLAC"; \
        RADIO_LIBS="$RADIO_LIBS -lFLAC"; \
        echo "[build] FLAC enabled"; \
    else echo "[build] FLAC not available"; fi; \
    if pkg-config --exists fdk-aac 2>/dev/null; then \
        RADIO_CFLAGS="$RADIO_CFLAGS -DHAVE_FDK_AAC"; \
        RADIO_LIBS="$RADIO_LIBS -lfdk-aac"; \
        echo "[build] AAC enabled"; \
    else echo "[build] fdk-aac not available"; fi; \
    echo "CFLAGS=$RADIO_CFLAGS LIBS=$RADIO_LIBS"; \
    g++ -O2 -std=c++17 -Wall $RADIO_CFLAGS -o soluna-radio radio.cpp $RADIO_LIBS

FROM ubuntu:22.04
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates libsqlite3-0 libcurl4 libssl3 ffmpeg libopus0 && \
    apt-get install -y --no-install-recommends libflac8 libfdk-aac2 2>/dev/null || true && \
    rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/soluna-relay /usr/local/bin/
COPY --from=builder /build/soluna-radio /usr/local/bin/
COPY start.sh /usr/local/bin/start.sh
RUN chmod +x /usr/local/bin/start.sh
EXPOSE 5100/udp
EXPOSE 5102/tcp

# Required env vars for production:
#   RELAY_ADMIN_KEY       — Admin API access key
#   RELAY_WEBHOOK_PATH    — Stripe webhook path suffix
#   RELAY_CHARGE_SECRET   — HMAC secret for UDP CHARGE commands

CMD ["/usr/local/bin/start.sh"]
