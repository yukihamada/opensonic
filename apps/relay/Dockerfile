FROM ubuntu:22.04 AS builder
RUN apt-get update && apt-get install -y g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY main.cpp .
RUN g++ -O2 -std=c++17 -o soluna-relay main.cpp -lpthread

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/soluna-relay /usr/local/bin/
EXPOSE 5100/udp
CMD ["soluna-relay", "--port", "5100", "--stats-interval", "60"]
