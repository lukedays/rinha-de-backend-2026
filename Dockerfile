# syntax=docker/dockerfile:1

# ---- build: binarios estaticos C + kernel ASM + indice (linux-amd64) ----
FROM gcc:14 AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends zlib1g-dev curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY c/ ./
RUN make clean >/dev/null 2>&1; \
    make rinha-server rinha-lb build_index \
      CFLAGS="-O3 -march=x86-64-v3 -mavx2 -mfma -flto -fno-plt -Wall" \
      LDFLAGS="-O3 -flto -static -pthread"

# gera o indice de forma reprodutivel (seed fixa) a partir do dataset oficial
ARG REFS_URL=https://raw.githubusercontent.com/zanfranceschi/rinha-de-backend-2026/main/resources/references.json.gz
RUN curl -fsSL "$REFS_URL" -o /tmp/refs.json.gz \
    && ./build_index /tmp/refs.json.gz /index.bin 2048 12 \
    && rm /tmp/refs.json.gz

# ---- runtime: scratch (binarios estaticos + indice) ----
FROM scratch AS runtime
COPY --from=build /src/rinha-server /rinha-server
COPY --from=build /src/rinha-lb /rinha-lb
COPY --from=build /index.bin /index.bin
ENV INDEX_PATH=/index.bin
# entrypoint definido por servico no docker-compose
