# ESP32-S3 超低功耗无线环境监测节点 —— 顶层 CMake 工程文件
#
# 目标芯片：ESP32-S3（DevKitC-1 v1.1 + WROOM-1-N8R8）
# 框架    ：ESP-IDF v5.4.3
#
# 编译方式（本机 PATH 上的 python 是 Microsoft Store 占位程序，
# 不能直接跑 export.ps1，必须用 tools/idf.ps1）：
#     .\tools\idf.ps1 build
#     .\tools\idf.ps1 -p COM4 flash monitor

cmake_minimum_required(VERSION 3.16)

# 我们自己的组件放在 firmware/components 下（与 AloT 工程同样布局）。
# 注意：EXTRA_COMPONENT_DIRS 指向一个**不存在**的目录时，ESP-IDF 会直接报
#   CMake Error at tools/cmake/project.cmake:472 (message):
#     Directory specified in EXTRA_COMPONENT_DIRS doesn't exist: ...
# 步骤 1 建骨架时 components/ 还是空的，所以这里加一层存在性判断，
# 这样"骨架能编译"不会因为"组件还没搬进来"而失败。
# （其实 project.cmake 也会自动扫描 ${CMAKE_CURRENT_LIST_DIR}/components，
#   这里显式声明只是为了可读性。）
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/components")
    set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(env_node)
