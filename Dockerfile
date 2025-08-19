FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive \
    WORKSPACE=/workspace \
    ZMQ_ROOT=/usr/local \
    OPENSSL_ROOT_DIR=/usr
# Install system packages including jq
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        gcc-11 g++-11 \
        pkg-config \
        libzmq3-dev \
        libssl-dev \
        git wget \
        python3 \
        python3-pip \
        clang-tidy-14 \
        jq && \
    rm -rf /var/lib/apt/lists/*
# Make GCC-11 the default compiler
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 100 && \
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-11 100
# Rest of your Dockerfile...
WORKDIR ${WORKSPACE}
COPY . .
# Create necessary directories for build and reports.
# No need to pre-create tests/data/usd_samples here if CMake is copying them or the tool reads from source.
RUN mkdir -p build_test tests/reports

RUN cmake -S . -B build_test -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_TESTS=ON \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_C_COMPILER=gcc-11 \
        -DCMAKE_CXX_COMPILER=g++-11 && \
    cmake --build build_test --parallel

# REMOVE: chmod +x for the shell script
# RUN chmod +x tests/scripts/comprehensive_test.sh

EXPOSE 5556
# Directly execute the usd_validation_tool, passing the test data directory as an argument.
# Make sure the path is correct within the container.
CMD ["/workspace/build_test/usd_validation_tool", "/workspace/tests/data/usd_samples"]
