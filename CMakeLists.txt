cmake_minimum_required(VERSION 3.14)
project(downloader-v2 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(DL_BUILD_TESTS "Build unit tests" ON)
option(DL_BUILD_CLI   "Build dlcli command line tool" ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# 可执行文件统一输出到 build/ 根目录（./build/download_server、./build/dlcli）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---- MySQL / MariaDB 客户端库（二选一安装即可）----
find_path(MYSQL_INCLUDE_DIR mysql.h
        PATHS /usr/include/mysql /usr/local/include/mysql
              /usr/include/mariadb /usr/local/include/mariadb)
find_library(MYSQL_LIB
        NAMES mysqlclient mariadb
        PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib /usr/lib64
              /usr/lib/x86_64-linux-gnu/mariadb /usr/lib/mariadb)

if (NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIB)
    message(FATAL_ERROR
        "未找到 mysql.h / libmysqlclient(或 libmariadb)，请先安装:\n"
        "  Ubuntu/Debian: sudo apt install libmysqlclient-dev libcurl4-openssl-dev build-essential cmake\n"
        "  (或: sudo apt install libmariadb-dev)")
endif()
message(STATUS "MySQL client: include=${MYSQL_INCLUDE_DIR} lib=${MYSQL_LIB}")

add_subdirectory(downloader-v2)

if (DL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS download_server RUNTIME DESTINATION bin)
