FROM ubuntu:22.04 AS builder
RUN apt-get update && apt-get install -y g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY main.cpp .
RUN g++ -O2 -std=c++17 -w -o soluna-relay main.cpp -lpthread

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/soluna-relay /usr/local/bin/
EXPOSE 5100/udp
EXPOSE 5102/tcp

# Required env vars for production:
#   RELAY_ADMIN_KEY       — Admin API access key
#   RELAY_WEBHOOK_PATH    — Stripe webhook path suffix
#   RELAY_CHARGE_SECRET   — HMAC secret for UDP CHARGE commands

CMD ["soluna-relay", "--port", "5100", "--stats-interval", "60", "--max-replay", "5000", "--wallets-db", "/data/wallets.json", "--transactions-log", "/data/transactions.jsonl", "--royalty-log", "/data/royalties.jsonl"]
