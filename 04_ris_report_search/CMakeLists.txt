cmake_minimum_required(VERSION 3.16)
project(ris_report_search CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    /usr/include
    /usr/local/include
)

find_package(Threads REQUIRED)

file(GLOB_RECURSE SRC_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

add_executable(ris_report_search ${SRC_FILES})

target_link_libraries(ris_report_search PRIVATE
    muduo_net
    muduo_base
    mysqlclient
    hiredis
    tinyxml2
    Threads::Threads
)
