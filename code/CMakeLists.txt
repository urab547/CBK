cmake_minimum_required(VERSION 3.20)
project(cbk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 可执行文件统一输出到 build/<preset>/bin，CI 和 GUI 才好找。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    # /utf-8 必须加：源文件里有中文注释，不加的话 MSVC 会按本地代码页解析，
    # 在非中文环境的机器上（比如 CI）直接编译报错。
    add_compile_options(/W4 /utf-8 /permissive-)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra)
endif()

option(CBK_BUILD_TESTS "构建单元测试" ON)

add_subdirectory(core)
add_subdirectory(cli)

if(CBK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
