FROM debian:13-slim AS builder
ARG MKPOOL_REF=v.0.3.0
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates \
    libboost-system-dev libboost-thread-dev libboost-program-options-dev \
    libssl-dev libpq-dev libpqxx-dev libzmq3-dev cppzmq-dev libsodium-dev libsecp256k1-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git clone --depth 1 --branch "${MKPOOL_REF}" https://github.com/Mecanik/mkpool.git .
RUN cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release -DMKPOOL_BUILD_TESTS=OFF \
    && cmake --build build -j2

FROM debian:13-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates python3 python3-psycopg2 \
    libboost-system1.83.0 libboost-thread1.83.0 libboost-program-options1.83.0 \
    libssl3t64 libpq5 libpqxx-7.10 libzmq5 libsodium23 libsecp256k1-1 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/yumlu
COPY --from=builder /src/build/mkpool /usr/local/bin/mkpool
COPY --from=builder /src/scripts/mkpool-ctl.py /usr/local/bin/mkpool-ctl.py
COPY scripts/generate-config.py /opt/yumlu/generate-config.py
COPY scripts/start-mkpool.sh /opt/yumlu/start-mkpool.sh
COPY dashboard /opt/yumlu/dashboard
RUN chmod +x /usr/local/bin/mkpool /usr/local/bin/mkpool-ctl.py /opt/yumlu/start-mkpool.sh
