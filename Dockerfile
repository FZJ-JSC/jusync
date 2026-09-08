FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive \
    WORKSPACE=/workspace \
    ZMQ_ROOT=/usr/local \
    OPENSSL_ROOT_DIR=/usr

# -------------- Install system dependencies --------------
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        gcc-11 g++-11 \
        pkg-config \
        libzmq3-dev \
        libssl-dev \
        libglm-dev \
        git wget \
        python3 \
        python3-pip \
        clang-tidy-14 \
        cppcheck \
        jq \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 100 && \
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-11 100

WORKDIR ${WORKSPACE}

# -------------- Copy and prepare source --------------
COPY . .

# Always ensure git submodules are up-to-date (including possible fixes upstream)
RUN git submodule update --init --recursive && \
    cd external/tinyusdz && git fetch origin && git checkout main || true && cd ../..

    RUN mkdir -p build_test tests/reports

    # -------------- Build project (with Ninja, C++17, and flags for large objects) --------------
    RUN cmake -S . -B build_test -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            -DBUILD_TESTS=ON \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
            -DCMAKE_C_COMPILER=gcc-11 \
            -DCMAKE_CXX_COMPILER=g++-11 \
            -DCMAKE_CXX_STANDARD=17 \
            -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections -Wno-error -O1" \
        && cmake --build build_test --parallel 4

    # -------------- Optional: Clang-Tidy static analysis --------------
    RUN find . \( -path './external' -o -path './external/*' -o -path './build*' -o -path './bin' \) -prune -o -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) -print0 | xargs -0 clang-tidy-14 -- \
        -I./include \
        -I./external/stb \
        -I./external/tinyusdz/src \
        -I./external/glm \
        -std=c++17 || true

    EXPOSE 5556

    CMD ["/workspace/build_test/usd_validation_tool", "/workspace/tests/data/usd_samples"]
    