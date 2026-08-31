# 核心静态库。
#
# ★ 这个库不允许有任何第三方依赖。★
# 只能用 C++ 标准库和 Windows SDK。课程规定扩展功能"直接"用第三方库实现，
# 该项扩展分记为原来的 50%。第三方依赖只能出现在 tests/ 和 gui/ 里。

add_library(cbk_core STATIC
    src/registry.cpp
    src/builtins.cpp

    # 新增的实现文件加在下面对应的分组里：
    # —— Z：基础 / 文件类型 / 元数据 ——
    # src/scanner.cpp
    # src/metadata.cpp
    # src/archive.cpp
    # src/engine.cpp
    # src/platform_win.cpp
    # src/packers/native_packer.cpp

    # —— 队友 A：打包 / 压缩 ——
    # src/packers/tar_packer.cpp
    # src/packers/cpio_packer.cpp
    # src/stages/huffman_stage.cpp
    # src/stages/lz77_stage.cpp

    # —— 队友 B：加密 ——
    # src/stages/xor_stage.cpp
    # src/stages/aes_stage.cpp
)

target_include_directories(cbk_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(cbk_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(cbk_core PUBLIC cxx_std_17)

if(WIN32)
    # 元数据那块要用到安全描述符 API，都在 advapi32 里。
    target_link_libraries(cbk_core PUBLIC advapi32)
endif()
