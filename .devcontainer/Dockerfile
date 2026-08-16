FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# 换源并安装 C++ 编译链、调试工具、构建工具及系统级开发库
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc \
    g++ \
    gdb \
    gdbserver \
    cmake \
    ninja-build \
    git \
    pkg-config \
    ca-certificates \
    curl \
    wget \
    # 常用依赖库（apt 官方库，下载快且稳定）
    libboost-all-dev \
    libssl-dev \
    libcurl4-openssl-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libmysqlclient-dev \
    libhiredis-dev \
    librabbitmq-dev \
    uuid-dev \
    net-tools \
    iputils-ping \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
