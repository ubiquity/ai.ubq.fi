# syntax=docker/dockerfile:1
ARG BOOKWORM=debian:12@sha256:268fb24c39bca92d326dd83ea3abebe4a3f7751c804ffef23bb6d857dd343ff8
FROM ${BOOKWORM} AS base
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl g++ make python3 && rm -rf /var/lib/apt/lists/*

FROM base AS toolchain
ARG JOBS=4
COPY cpp/gateway/packaging/portable/dependencies.sh /usr/local/bin/portable-dependencies
RUN bash /usr/local/bin/portable-dependencies "$JOBS"

FROM toolchain AS cpp
ARG JOBS=4
RUN apt-get update && apt-get install -y --no-install-recommends ccache && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY cpp/gateway/CMakeLists.txt cpp/gateway/PORT-MANIFEST.json cpp/gateway/
COPY cpp/gateway/cmake cpp/gateway/cmake
COPY cpp/gateway/tools cpp/gateway/tools
COPY cpp/gateway/include cpp/gateway/include
COPY cpp/gateway/src cpp/gateway/src
COPY cpp/gateway/third_party cpp/gateway/third_party
COPY cpp/gateway/runtime cpp/gateway/runtime
COPY cpp/gateway/entrypoints cpp/gateway/entrypoints
COPY cpp/gateway/bench cpp/gateway/bench
COPY cpp/gateway/fuzz cpp/gateway/fuzz
COPY cpp/gateway/tests/legacy/runtime_http_llhttp_test.cpp cpp/gateway/tests/legacy/passkeys_asn1_library_test.cpp cpp/gateway/tests/legacy/
COPY cpp/gateway/tests/production_smoke/serve_failure_gate.cpp cpp/gateway/tests/production_smoke/
COPY cpp/gateway/packaging/portable/clang-warnings.txt /opt/uos/clang-warnings.txt
ENV CCACHE_DIR=/var/cache/ccache CCACHE_COMPILERCHECK=content CCACHE_EXTRAFILES=/opt/uos/clang-warnings.txt
ARG FRESH_COMPILE=0
RUN --network=none --mount=type=cache,target=/var/cache/ccache \
    if [ "$FRESH_COMPILE" = 1 ]; then export CCACHE_DISABLE=1; fi; \
    UOS_GATEWAY_BUILD_EVIDENCE=bookworm-arm64 UOS_GATEWAY_TOOL_ROOT=/opt/uos \
    cmake -S cpp/gateway -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 -DUOS_GATEWAY_FRAME_RECYCLING_DISABLED=ON \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      '-DCMAKE_C_FLAGS=-march=armv8-a -ffile-prefix-map=/source=.' \
      '-DCMAKE_CXX_FLAGS=-march=armv8-a -isystem /opt/uos/include -ffile-prefix-map=/source=. --warning-suppression-mappings=/opt/uos/clang-warnings.txt' \
      '-DCMAKE_C_FLAGS_RELEASE=-O2 -g0' '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -g0' \
      '-DCMAKE_EXE_LINKER_FLAGS=-L/opt/uos/lib -static-libstdc++ -static-libgcc' \
      '-DCMAKE_CXX_STANDARD_LIBRARIES=/opt/uos/lib/libicudata.a -lm' \
    && cmake --build /build --parallel "$JOBS" --target uos_gateway uos_kv_convert uos_kv_proof \
    && ccache --show-stats

FROM base AS native
RUN apt-get update && apt-get install -y --no-install-recommends libnode-dev patch unzip xz-utils \
    && rm -rf /var/lib/apt/lists/*
COPY cpp/gateway/packaging/portable/native.sh /usr/local/bin/portable-native
COPY cpp/gateway/packaging/portable/native-receipt.py /usr/local/bin/portable-native-receipt
COPY cpp/gateway/packaging/portable/licenses /usr/local/share/uos-crate-licenses
COPY cpp/gateway/packaging/native-kv /source/cpp/gateway/packaging/native-kv
COPY cpp/gateway/packaging/confinement/permissions-native.c /source/cpp/gateway/packaging/confinement/
RUN bash /usr/local/bin/portable-native

FROM cpp AS assemble
COPY --from=native /native /native
COPY static static
COPY favicon.png favicon-32.png ./
COPY cpp/gateway/packaging/native-kv/inputs.json cpp/gateway/packaging/native-kv/
COPY cpp/gateway/docs/licenses cpp/gateway/docs/licenses
COPY cpp/gateway/docs/RASPBERRY-PI.md cpp/gateway/docs/
COPY cpp/gateway/ops/launch-native.sh cpp/gateway/ops/
COPY cpp/gateway/packaging/SQLITE-LICENSE cpp/gateway/packaging/
COPY cpp/gateway/packaging/portable cpp/gateway/packaging/portable
ARG SOURCE_COMMIT
ARG SOURCE_TREE
ARG SOURCE_COMMIT_EPOCH
RUN --network=none python3 -B cpp/gateway/packaging/portable/payload.py "$SOURCE_COMMIT" "$SOURCE_TREE" "$SOURCE_COMMIT_EPOCH"

FROM scratch AS package
COPY --from=assemble /release/ /
