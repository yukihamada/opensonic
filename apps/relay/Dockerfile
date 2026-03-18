FROM ubuntu:22.04 AS builder
RUN apt-get update && apt-get install -y g++ libsqlite3-dev libcurl4-openssl-dev libssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY main.cpp .
RUN g++ -O2 -std=c++17 -Wall -o soluna-relay main.cpp -lpthread -lsqlite3 -lcurl -lssl -lcrypto

COPY radio.cpp .
RUN g++ -O2 -std=c++17 -Wall -o soluna-radio radio.cpp -lpthread

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libsqlite3-0 libcurl4 libssl3 ffmpeg && rm -rf /var/lib/apt/lists/*
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
