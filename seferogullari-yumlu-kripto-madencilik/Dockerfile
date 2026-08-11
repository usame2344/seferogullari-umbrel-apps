FROM debian:trixie AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    libboost-system-dev \
    libboost-thread-dev \
    libboost-program-options-dev \
    libssl-dev \
    libpq-dev \
    libpqxx-dev \
    libzmq3-dev \
    cppzmq-dev \
    libsodium-dev \
    libsecp256k1-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY mkpool-src/ /build/

RUN cmake -S . -B build \
    -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMKPOOL_BUILD_TESTS=OFF \
    && cmake --build build -j2


FROM debian:trixie

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    python3 \
    python3-psycopg2 \
    libboost-system-dev \
    libboost-thread-dev \
    libboost-program-options-dev \
    libssl-dev \
    libpq-dev \
    libpqxx-dev \
    libzmq3-dev \
    libsodium-dev \
    libsecp256k1-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/yumlu

COPY --from=builder /build/build/mkpool /usr/local/bin/mkpool
COPY mkpool-src/scripts/mkpool-ctl.py /opt/yumlu/mkpool-src/scripts/mkpool-ctl.py
COPY dashboard/ /opt/yumlu/dashboard/
COPY scripts/ /opt/yumlu/scripts/

RUN chmod +x \
    /usr/local/bin/mkpool \
    /opt/yumlu/mkpool-src/scripts/mkpool-ctl.py \
    /opt/yumlu/scripts/*.sh

