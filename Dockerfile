# syntax=docker/dockerfile:1

# The dashboard is a static bundle with no runtime dependency on the engine image, so it builds
# in its own stage. Bun rather than a Node image: the app's toolchain is Bun end to end, from
# typecheck through the Vite production build.
FROM oven/bun:1.3.5 AS web

WORKDIR /web
# Dependencies first, so editing a panel does not re-resolve the tree.
COPY apps/web/package.json apps/web/bun.lock ./
RUN bun install --frozen-lockfile
COPY apps/web/ ./
RUN bun run build

FROM nvidia/cuda:13.1.2-devel-ubuntu24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        cmake \
        curl \
        libavcodec-dev \
        libavformat-dev \
        libavutil-dev \
        libcurl4-openssl-dev \
        libswscale-dev \
        ninja-build \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
# Only the C++ applications. apps/web is not part of the CMake build, and copying it here would
# invalidate this cached CUDA layer on every dashboard edit.
COPY apps/CMakeLists.txt apps/
COPY apps/cli/ apps/cli/
COPY apps/perplexity/ apps/perplexity/
COPY apps/serve/ apps/serve/
COPY include/ include/
COPY src/ src/
COPY third_party/ third_party/

RUN --mount=type=cache,id=ninfer-sm89-cuda13.1-release,target=/build,sharing=locked \
    cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DNINFER_BUILD_APPS=ON \
        -DBUILD_TESTING=OFF \
        -DNINFER_BUILD_BENCHMARKS=OFF \
    && cmake --build /build --parallel 16 --target ninfer ninfer-serve \
    && install -D /build/apps/ninfer /opt/ninfer/bin/ninfer \
    && install -D /build/apps/ninfer-serve /opt/ninfer/bin/ninfer-serve

FROM nvidia/cuda:13.1.2-runtime-ubuntu24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        curl \
        libavcodec60 \
        libavformat60 \
        libavutil58 \
        libcurl4t64 \
        libswscale7 \
    && rm -rf /var/lib/apt/lists/*

# The CUDA runtime image ships forward-compatibility libraries in
# /usr/local/cuda*/compat (a newer libcuda.so than the host driver). Forward
# compatibility is supported only on datacenter GPUs; on any GeForce card the
# loader picks these up and every CUDA call fails at startup with
#   cudaErrorCompatNotSupportedOnDevice: forward compatibility was attempted
#   on non supported HW
# Removing them lets the container use the host driver through ordinary CUDA
# minor-version compatibility, which is what an RTX 3090/3090 Ti/4090 needs.
RUN rm -rf /usr/local/cuda-13.1/compat /usr/local/cuda-13/compat /usr/local/cuda/compat

ARG MODEL_URL=https://huggingface.co/neroued/Qwen3.8-27B-NInfer/resolve/dc370fb6295ae8b786e1af4f90d7142a16255c35/qwen3_8_27b.ninfer
ARG MODEL_CONTEXT_PATH=models/qwen3_8_27b_dflash2.ninfer
ARG MODEL_SHA256=0634abb07024221de141456cf04a42ab74b18bc38e1b781c6eb2e062a467eec3
# Keep immutable model bytes below the frequently changing application layers.
RUN --mount=type=bind,source=.,target=/context,readonly \
    mkdir -p /opt/ninfer/models \
    && if [ -f "/context/${MODEL_CONTEXT_PATH}" ]; then \
         cp "/context/${MODEL_CONTEXT_PATH}" /opt/ninfer/models/qwen3_8_27b.ninfer; \
       else \
         curl --fail --location --retry 5 \
           --output /opt/ninfer/models/qwen3_8_27b.ninfer \
           "$MODEL_URL"; \
       fi \
    && printf '%s  %s\n' \
         "$MODEL_SHA256" \
         /opt/ninfer/models/qwen3_8_27b.ninfer \
       | sha256sum --check

COPY --from=build /opt/ninfer/bin/ninfer /usr/local/bin/ninfer
COPY --from=build /opt/ninfer/bin/ninfer-serve /usr/local/bin/ninfer-serve
COPY --from=web /web/dist /opt/ninfer/web

WORKDIR /workspace
EXPOSE 8080
STOPSIGNAL SIGTERM
VOLUME ["/var/cache/ninfer"]

# Production RTX 4090 profile: the DFlash2 companion artifact, K7 proposal window and CUDA
# Graph decode. 176128 tokens leaves 134 MiB planned slack in the complete serving process;
# vision stays opt-in because its workspace would require a materially smaller context budget.
CMD ["ninfer-serve", "/opt/ninfer/models/qwen3_8_27b.ninfer", "--model-id", "qwen3.8-27b", "--host", "0.0.0.0", "--port", "8080", "--max-context", "176128", "--kv-capacity", "176128", "--max-concurrency", "1", "--max-pending-requests", "16", "--pending-timeout-ms", "600000", "--prefill-chunk", "1024", "--kv-dtype", "rk4v4-e8", "--spec", "dflash2", "--draft-tokens", "7", "--lm-head-draft", "--preserve-thinking", "--request-log-jsonl", "/var/cache/ninfer/request-log.jsonl", "--web-dir", "/opt/ninfer/web"]
