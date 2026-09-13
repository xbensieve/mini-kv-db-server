FROM gcc:12 AS builder

WORKDIR /app
COPY Makefile ./
COPY include/ ./include/
COPY src/ ./src/
RUN make bin/mini-kv

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y netcat-openbsd && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/bin/mini-kv /usr/local/bin/mini-kv

RUN mkdir -p /app/data

EXPOSE 8888

ENTRYPOINT ["mini-kv"]
CMD ["--port", "8888"]
