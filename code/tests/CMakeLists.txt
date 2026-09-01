# 单元测试。
#
# GoogleTest 是测试工具，不是"用第三方库实现扩展功能"，不扣分——
# 评分表里反而有"是否使用第三方测试工具"这一项。

include(FetchContent)

# 用 git 按 tag 拉，不用 URL + URL_HASH。
# 原因：GitHub 为 tag 自动生成的 .zip / .tar.gz 不保证字节稳定，
# 它那边重新生成一次哈希就变了，CI 会突然挂在"Hash mismatch"上
# （本项目已经踩过一次）。git 自己就保证完整性，不需要额外对哈希。
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)
# MSVC 上必须加这句，否则 gtest 用的运行时库和我们的不一致，链接会失败。
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# 网络连不上 GitHub 时的两个办法（校园网偶尔会这样）：
#   1. 先手动 clone 一份，再配置时指过去：
#      git clone --depth 1 --branch v1.15.2 https://github.com/google/googletest D:\\gtest
#      cmake --preset msvc -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=D:/gtest
#   2. 临时不编测试：cmake --preset msvc -DCBK_BUILD_TESTS=OFF
# CI 跑在 GitHub 自己的机器上，不会有这个问题。

add_executable(cbk_tests
    unit/registry_test.cpp
    unit/event_test.cpp
    unit/path_test.cpp
    unit/byte_io_test.cpp
    unit/entry_codec_test.cpp
    unit/packer_roundtrip_test.cpp
    unit/archive_test.cpp
    unit/scanner_test.cpp
    unit/metadata_test.cpp
    unit/engine_test.cpp

    # 新增测试文件加在下面：
    # unit/stage_roundtrip_test.cpp    # 队友 A/B：随机字节正变换再逆变换
)

target_link_libraries(cbk_tests PRIVATE cbk_core GTest::gtest_main)

# core/src 是内部实现目录，对 cbk_core 之外是不可见的。测试要白盒验证
# platform_win 这类内部组件，所以把 core/ 加进来，用 "src/xxx.h" 引用。
# 只有测试有这个特权——cli 和 gui 只能用 include/cbk 下的对外契约。
#
# 再加上 tests/ 自己，这样测试之间共用的辅助头写成 "unit/temp_dir.h"，
# 一眼看得出是测试专用的东西。
target_include_directories(cbk_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}
)

include(GoogleTest)
gtest_discover_tests(cbk_tests)
