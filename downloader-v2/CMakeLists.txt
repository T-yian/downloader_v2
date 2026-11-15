# ============ 基础库：零第三方依赖（单测只依赖它） ============
add_library(dlsvc_base STATIC
        src/log.cpp
        src/config.cpp
        src/util.cpp
        src/sha1.cpp
        src/mini_json.cpp
        src/memory_store.cpp)
target_include_directories(dlsvc_base PUBLIC include)
target_link_libraries(dlsvc_base PUBLIC Threads::Threads)

# ============ 下载内核（libcurl） ============
add_library(dlsvc_core STATIC
        src/core/http_client.cpp
        src/core/download_runner.cpp)
target_include_directories(dlsvc_core PUBLIC ${CURL_INCLUDE_DIRS} include)
target_link_libraries(dlsvc_core PUBLIC dlsvc_base ${CURL_LIBRARIES})

# ============ DB 层（MySQL C API：连接池 + 双表账本 DAO） ============
# mysql_store 是 header-only（MysqlTaskStore 全部内联在 mysql_store.h）
add_library(dlsvc_db STATIC
        src/db/db_pool.cpp
        src/db/task_dao.cpp
        src/db/segment_dao.cpp)
target_include_directories(dlsvc_db PUBLIC ${MYSQL_INCLUDE_DIR} include)
target_link_libraries(dlsvc_db PUBLIC dlsvc_base ${MYSQL_LIB})

# ============ 调度层 + 接入层（cpp-httplib header-only） ============
add_library(dlsvc_svc STATIC
        src/scheduler/progress_flusher.cpp
        src/scheduler/task_manager.cpp
        src/api/http_api.cpp)
target_include_directories(dlsvc_svc PRIVATE third_party/httplib)
target_link_libraries(dlsvc_svc PUBLIC dlsvc_base dlsvc_core dlsvc_db)

add_executable(download_server src/main_server.cpp)
target_link_libraries(download_server PRIVATE dlsvc_svc)

if (DL_BUILD_CLI)
    add_executable(dlcli src/main_cli.cpp)
    target_link_libraries(dlcli PRIVATE dlsvc_core)
endif()
