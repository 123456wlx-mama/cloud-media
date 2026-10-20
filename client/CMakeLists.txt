# 客户端（Qt 5.12 Widgets）：Phase 8 起从占位 exe 升级为三段式骨架。
# 工具链必须显式锁 Qt 自带的 MinGW 7.3 32 位（见 docs/plans/phase-8.md）。
find_package(Qt5 COMPONENTS Widgets Network Sql REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(cloud-media-client
    app/main.cpp
    ui/MainWindow.cpp
    ui/MainWindow.h
    ui/LoginDialog.cpp
    ui/LoginDialog.h
    ui/resources.qrc
)

target_include_directories(cloud-media-client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cloud-media-client PRIVATE
    cm_protocol
    cm_compile_options
    Qt5::Widgets
    Qt5::Network
    Qt5::Sql
)
set_target_properties(cloud-media-client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
