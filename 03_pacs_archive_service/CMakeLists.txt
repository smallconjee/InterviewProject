cmake_minimum_required(VERSION 3.16)
project(pacs_archive_service CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/proto
    /usr/include
    /usr/local/include
)

find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# 共享基础源码：配置/日志/连接池（三个二进制都用）
file(GLOB COMMON_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/db/*.cpp
)

# 归档主服务（导入/查询/网关）：src 全部源码 + proto 生成的 pb（AuthGate 调用认证服务）
file(GLOB_RECURSE PACS_MAIN_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)
add_executable(pacs_archive_service ${PACS_MAIN_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/auth.pb.cc)

target_link_libraries(pacs_archive_service PRIVATE
    wfrest workflow srpc mysqlclient rabbitmq SimpleAmqpClient cpp-sdk ppconsul
    protobuf::libprotobuf lz4 snappy
    CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads
)

# 备份消费者（独立进程：可单独重启，演示消息不丢不重）
file(GLOB OSS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/oss/*.cpp)
add_executable(pacs_backup_consumer
    ${CMAKE_CURRENT_SOURCE_DIR}/consumer/main.cpp
    ${COMMON_SRC}
    ${OSS_SRC})

target_link_libraries(pacs_backup_consumer PRIVATE
    workflow mysqlclient rabbitmq SimpleAmqpClient cpp-sdk
    protobuf::libprotobuf lz4 snappy
    CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

# 认证微服务（srpc + Protobuf + Consul + PBKDF2 + JWT）
add_executable(pacs_auth_service
    ${CMAKE_CURRENT_SOURCE_DIR}/auth/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/auth/Jwt.cpp
    ${COMMON_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/auth.pb.cc)

target_link_libraries(pacs_auth_service PRIVATE
    srpc workflow ppconsul mysqlclient
    protobuf::libprotobuf lz4 snappy
    CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
