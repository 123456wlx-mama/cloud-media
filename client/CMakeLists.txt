# 客户端（Qt 5.12 Widgets）：P8-2 起接传输中心（transfer + sqlapi）。
# 工具链必须显式锁 Qt 自带的 MinGW 7.3 32 位（见 docs/plans/phase-8.md）。
find_package(Qt5 COMPONENTS Widgets Network Sql REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CM_CLIENT_NETAPI_SOURCES
    netapi/FrameAssembler.cpp
    netapi/FrameAssembler.h
    netapi/NetWorker.cpp
    netapi/NetWorker.h
    netapi/NetClient.cpp
    netapi/NetClient.h
)
set(CM_CLIENT_TRANSFER_SOURCES
    transfer/TransferTask.h
    transfer/TransferQueue.cpp
    transfer/TransferQueue.h
)
set(CM_CLIENT_SQLAPI_SOURCES
    sqlapi/TaskStore.cpp
    sqlapi/TaskStore.h
)
set(CM_CLIENT_FLOW_SOURCES
    shareapi/ShareFlow.cpp
    shareapi/ShareFlow.h
    recycleapi/RecycleFlow.cpp
    recycleapi/RecycleFlow.h
)

add_executable(cloud-media-client
    app/main.cpp
    app/smoke.cpp
    app/smoke.h
    ${CM_CLIENT_NETAPI_SOURCES}
    ${CM_CLIENT_TRANSFER_SOURCES}
    ${CM_CLIENT_SQLAPI_SOURCES}
    ${CM_CLIENT_FLOW_SOURCES}
    ui/MainWindow.cpp
    ui/MainWindow.h
    ui/LoginDialog.cpp
    ui/LoginDialog.h
    ui/FileTableModel.cpp
    ui/FileTableModel.h
    ui/TransferPane.cpp
    ui/TransferPane.h
    ui/Toast.cpp
    ui/Toast.h
    ui/ShareDialog.cpp
    ui/ShareDialog.h
    ui/RecycleDialog.cpp
    ui/RecycleDialog.h
    ui/resources.qrc
)

target_include_directories(cloud-media-client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cloud-media-client PRIVATE
    cm_protocol
    cm_common
    cm_compile_options
    Qt5::Widgets
    Qt5::Network
    Qt5::Sql
)
set_target_properties(cloud-media-client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
