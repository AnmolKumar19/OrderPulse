# Multi-stage build: compile in a full toolchain image, run in a lean one.
# Builds only the web server target -- tests and the latency benchmark
# aren't part of the deployed image (see .dockerignore and the
# ORDERPULSE_BUILD_* options below).

FROM ubuntu:24.04 AS build

RUN apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends cmake g++ make ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DORDERPULSE_BUILD_TESTS=OFF -DORDERPULSE_BUILD_BENCH=OFF -DORDERPULSE_BUILD_SERVER=ON && \
    cmake --build build -j --target orderpulse_server

FROM ubuntu:24.04 AS runtime

# libstdc++6 for the dynamically-linked binary; nothing else -- no compiler,
# no headers, no build tooling in the shipped image.
RUN apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends libstdc++6 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/web/server/orderpulse_server /app/orderpulse_server
COPY web/public /app/web/public

# Render sets $PORT at runtime; server_main.cpp reads it (default 8080 if
# unset, e.g. for `docker run -p 8080:8080` locally with no PORT passed).
ENV PORT=8080
EXPOSE 8080

CMD ["/app/orderpulse_server"]
