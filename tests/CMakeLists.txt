# 单测只依赖 dlsvc_base（零第三方依赖），任何装了编译器的 Linux 都能跑

macro(add_dl_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE dlsvc_base)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

add_dl_test(test_blocking_queue)
add_dl_test(test_mini_json)
add_dl_test(test_util)
add_dl_test(test_sha1)
add_dl_test(test_memory_store)
