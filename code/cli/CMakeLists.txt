# 命令行程序 cbk.exe —— GUI 通过启动这个进程来干活。

add_executable(cbk
    main.cpp
)

target_link_libraries(cbk PRIVATE cbk_core)
