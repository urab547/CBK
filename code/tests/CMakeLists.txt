# 单元测试。
#
# GoogleTest 是测试工具，不是"用第三方库实现扩展功能"，不扣分——
# 评分表里反而有"是否使用第三方测试工具"这一项。

include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
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

    # 新增测试文件加在下面：
    # unit/packer_roundtrip_test.cpp   # 队友 A：手捏 EntryMeta 打包再解包
    # unit/stage_roundtrip_test.cpp    # 队友 A/B：随机字节正变换再逆变换
    # unit/path_test.cpp               # Z：长路径与中文路径
    # unit/metadata_test.cpp           # Z：属性、时间戳、SDDL 往返
)

target_link_libraries(cbk_tests PRIVATE cbk_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cbk_tests)
