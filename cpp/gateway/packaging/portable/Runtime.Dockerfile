ARG DEBIAN=debian:12
FROM ${DEBIAN}
ARG SSL=libssl3
RUN apt-get update && apt-get install -y --no-install-recommends \
      "$SSL" ca-certificates zlib1g libbrotli1 libzstd1 python3 binutils \
      && rm -rf /var/lib/apt/lists/*
