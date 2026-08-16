#!/bin/bash
# ============================================================================
# install_deps.sh — 容器内编译安装全部第三方依赖（优先 00_third_party 本地源码）
# 幂等：已安装的库自动跳过（检查头文件/库文件是否存在）
# 依赖顺序：workflow 必须先于 wfrest/srpc；rabbitmq-c 先于 SimpleAmqpClient
# ============================================================================
set -e

TP=/workspace/00_third_party
NPROC=$(nproc)
LOG_DIR=/tmp/deps_build
mkdir -p "$LOG_DIR"

installed() { # installed <探测文件...> —— 任一不存在则未安装
    for f in "$@"; do [ -e "$f" ] || return 1; done
    return 0
}

build_and_install() { # build_and_install <目录名> <cmake额外参数...>
    local name=$1; shift
    echo "--> [$name] 编译安装中..."
    rm -rf "/tmp/build_$name"
    cp -r "$TP/$name" "/tmp/build_$name"
    # CMAKE_POLICY_DEFAULT_CMP0048=NEW：兼容老库的无 cmake_minimum_required 写法
    (cd "/tmp/build_$name" && cmake -B build -D CMAKE_POLICY_DEFAULT_CMP0048=NEW "$@" > "$LOG_DIR/$name.log" 2>&1 \
        && cmake --build build -j"$NPROC" >> "$LOG_DIR/$name.log" 2>&1 \
        && cmake --install build >> "$LOG_DIR/$name.log" 2>&1) \
        || { echo "[$name] 构建失败，日志: $LOG_DIR/$name.log"; tail -30 "$LOG_DIR/$name.log"; return 1; }
    rm -rf "/tmp/build_$name"
    echo "--> [$name] 安装完成"
}

echo "=== 开始安装第三方依赖 ==="

# ---------- 0. muduo（RIS 的网络库；ARM64 需修 long double 断言） ----------
if installed /usr/local/include/muduo/net/TcpServer.h; then
    echo "--> [muduo] 已安装，跳过"
else
    rm -rf /tmp/build_muduo
    cp -r "$TP/muduo-2.0.2" /tmp/build_muduo
    (cd /tmp/build_muduo \
        && sed -i 's/-Werror//g' CMakeLists.txt \
        && sed -i 's/const int kMaxNumericSize = 32;/const int kMaxNumericSize = 48;/' muduo/base/LogStream.h \
        && cmake -B build -D MUDUO_BUILD_EXAMPLES=OFF -D CMAKE_POLICY_DEFAULT_CMP0048=NEW > "$LOG_DIR/muduo.log" 2>&1 \
        && cmake --build build -j"$NPROC" >> "$LOG_DIR/muduo.log" 2>&1 \
        && cmake --install build >> "$LOG_DIR/muduo.log" 2>&1) \
        || { echo "[muduo] 构建失败"; tail -30 "$LOG_DIR/muduo.log"; exit 1; }
    rm -rf /tmp/build_muduo
    echo "--> [muduo] 安装完成"
fi

# ---------- 1. Sogou Workflow（wfrest/srpc 的前置） ----------
if installed /usr/local/include/workflow/WFHttpServer.h /usr/local/lib/libworkflow.a; then
    echo "--> [workflow] 已安装，跳过"
else
    rm -rf /tmp/build_workflow_src
    cp -r "$TP/workflow" /tmp/build_workflow_src
    (cd /tmp/build_workflow_src \
        && cmake -B build -D CMAKE_BUILD_TYPE=Release > "$LOG_DIR/workflow.log" 2>&1 \
        && cmake --build build -j"$NPROC" >> "$LOG_DIR/workflow.log" 2>&1 \
        && cmake --install build >> "$LOG_DIR/workflow.log" 2>&1) \
        || { echo "[workflow] 构建失败"; tail -30 "$LOG_DIR/workflow.log"; exit 1; }
    rm -rf /tmp/build_workflow_src
    echo "--> [workflow] 安装完成"
fi

# ---------- 2. wfrest（workflow 之上的 HTTP 框架） ----------
if installed /usr/local/include/wfrest/HttpServer.h; then
    echo "--> [wfrest] 已安装，跳过"
else
    build_and_install wfrest -D CMAKE_BUILD_TYPE=Release
fi

# ---------- 3. rabbitmq-c（SimpleAmqpClient 前置；apt 的版本也可，这里用源码保版本一致） ----------
if installed /usr/local/include/amqp.h; then
    echo "--> [rabbitmq-c] 已安装，跳过"
else
    build_and_install rabbitmq-c-0.11.0 -D CMAKE_BUILD_TYPE=Release \
        -D ENABLE_SSL_SUPPORT=OFF -D BUILDexamples=OFF -D BUILDTOOLS=OFF -D BUILD_TESTING=OFF -D BUILD_API_DOCS=OFF
fi

# ---------- 4. SimpleAmqpClient（rabbitmq-c 的 C++ 封装，消费者用） ----------
if installed /usr/local/include/SimpleAmqpClient/SimpleAmqpClient.h; then
    echo "--> [SimpleAmqpClient] 已安装，跳过"
else
    # 2.5.1 的 CMake 用 find_package(Rabbitmqc)；rabbitmq-c 装到 /usr/local 后需要显式指路
    build_and_install SimpleAmqpClient-2.5.1 -D CMAKE_BUILD_TYPE=Release \
        -D Rabbitmqc_ROOT=/usr/local -DENABLE_TESTING=OFF
fi

# ---------- 5. 阿里云 OSS C++ SDK（需要 curl + openssl） ----------
# 注意：该 SDK 把 CMAKE_INSTALL_PREFIX 硬编码为 /alibabacloud，
# 安装后把头文件搬到 /usr/local/include/alibabacloud，库产物为 libcpp-sdk.a
if installed /usr/local/include/alibabacloud/oss/OssClient.h; then
    echo "--> [oss-sdk] 已安装，跳过"
else
    rm -rf /tmp/build_oss
    cp -r "$TP/aliyun-oss-cpp-sdk-1.10.0" /tmp/build_oss
    (cd /tmp/build_oss/sdk \
        && cmake -B build -D CMAKE_BUILD_TYPE=Release -D BUILD_SAMPLE=OFF -D BUILD_TEST=OFF \
           -D CMAKE_POLICY_DEFAULT_CMP0048=NEW > "$LOG_DIR/oss.log" 2>&1 \
        && cmake --build build -j"$NPROC" >> "$LOG_DIR/oss.log" 2>&1 \
        && cmake --install build >> "$LOG_DIR/oss.log" 2>&1) \
        || { echo "[oss-sdk] 构建失败，日志: $LOG_DIR/oss.log"; tail -30 "$LOG_DIR/oss.log"; exit 1; }
    rm -rf /tmp/build_oss
    rm -rf /usr/local/include/alibabacloud
    mv /alibabacloud /usr/local/include/alibabacloud
    echo "--> [oss-sdk] 安装完成（头文件已归位 /usr/local/include/alibabacloud，库名 cpp-sdk）"
fi

# ---------- 6. ppconsul（Consul 注册/发现，需要 curl + boost::regex） ----------
if installed /usr/local/include/ppconsul/ppconsul.h; then
    echo "--> [ppconsul] 已安装，跳过"
else
    build_and_install ppconsul-0.2.3 -D CMAKE_BUILD_TYPE=Release -D PPCONSUL_BUILD_EXAMPLES=OFF -D PPCONSUL_BUILD_TESTS=OFF
fi

# ---------- 7. srpc（workflow 之上的 RPC 框架，认证服务用；依赖 lz4/snappy） ----------
if installed /usr/local/include/srpc/rpc_define.h; then
    echo "--> [srpc] 已安装，跳过"
else
    apt-get update -qq && apt-get install -y -qq liblz4-dev libsnappy-dev > /dev/null 2>&1 || true
    build_and_install srpc-0.10.2 -D CMAKE_BUILD_TYPE=Release
fi

# ---------- 8. tinyxml2（RIS 报告解析） ----------
if installed /usr/local/include/tinyxml2.h; then
    echo "--> [tinyxml2] 已安装，跳过"
else
    build_and_install tinyxml2-master -D CMAKE_BUILD_TYPE=Release -D tinyxml2_BUILD_TESTING=OFF
fi

# ---------- 9. cppjieba（header-only）+ 词典 ----------
if installed /usr/local/include/cppjieba/Jieba.hpp /usr/local/dict/jieba.dict.utf8; then
    echo "--> [cppjieba] 已安装，跳过"
else
    cp -r "$TP/cppjieba/include/cppjieba" /usr/local/include/
    mkdir -p /usr/local/include/cppjieba/limonp
    cp -r "$TP/cppjieba/deps/limonp/include/limonp/"* /usr/local/include/cppjieba/limonp/ 2>/dev/null || true
    cp -r "$TP/cppjieba/dict" /usr/local/dict
    echo "--> [cppjieba] 头文件与词典安装完成"
fi

# ---------- 10. utfcpp（header-only） ----------
if installed /usr/local/include/utfcpp/utf8.h; then
    echo "--> [utfcpp] 已安装，跳过"
else
    mkdir -p /usr/local/include/utfcpp
    cp -r "$TP/utfcpp-4.0.6/source/"* /usr/local/include/utfcpp/
    echo "--> [utfcpp] 安装完成"
fi

# ---------- 11. nlohmann/json（单头文件） ----------
if installed /usr/local/include/nlohmann/json.hpp; then
    echo "--> [nlohmann] 已安装，跳过"
else
    mkdir -p /usr/local/include/nlohmann
    SRC=$(find "$TP/nlohmann" -name json.hpp -path "*single_include*" | head -1)
    [ -z "$SRC" ] && SRC=$(find "$TP/nlohmann" -name json.hpp | head -1)
    cp "$SRC" /usr/local/include/nlohmann/json.hpp
    echo "--> [nlohmann] 安装完成"
fi

ldconfig
echo "=== 全部依赖就绪：workflow/wfrest/rabbitmq-c/SimpleAmqpClient/oss-sdk/ppconsul/srpc/tinyxml2/cppjieba/utfcpp/nlohmann ==="
