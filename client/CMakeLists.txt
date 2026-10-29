# 客户端（Qt 5.12 Widgets）：P8-2 起接传输中心（transfer + sqlapi）。
# 工具链必须显式锁 Qt 自带的 MinGW 7.3 32 位（见 docs/plans/phase-8.md）。
find_package(Qt5 COMPONENTS Widgets Network Sql REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
add_subdirectory(player)

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
    transfer/FolderUploader.cpp
    transfer/FolderUploader.h
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
    ui/PlayerPane.cpp
    ui/PlayerPane.h
    ui/VideoSurface.cpp
    ui/VideoSurface.h
    ui/GlVideoSurface.cpp
    ui/GlVideoSurface.h
    ui/PainterVideoSurface.cpp
    ui/PainterVideoSurface.h
    ui/Letterbox.h
    ui/Toast.cpp
    ui/Toast.h
    ui/EmptyState.cpp
    ui/EmptyState.h
    ui/ShareDialog.cpp
    ui/ShareDialog.h
    ui/RecycleDialog.cpp
    ui/RecycleDialog.h
    ui/RegisterDialog.cpp
    ui/RegisterDialog.h
    ui/resources.qrc
)

target_include_directories(cloud-media-client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cloud-media-client PRIVATE
    cm_protocol
    cm_common
    cm_player_core
    cm_compile_options
    Qt5::Widgets
    Qt5::Network
    Qt5::Sql
)
set_target_properties(cloud-media-client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(WIN32)
    # 构建即部署：Qt DLL/插件 + 32 位编译器运行时，全部拷到 exe 旁。
    # windeployqt 必须加 --no-compiler-runtime：它按 PATH 找编译器运行时，
    # PATH 里有 64 位 MinGW 时会拷 libgcc_s_seh-1（64 位）→ 32 位 exe 0xc000007b。
    # 三个运行时 DLL 改从本构建的编译器目录拷（与 exe 同位数，必然正确）。
    get_filename_component(_qt_bin "${Qt5_DIR}/../../../bin" ABSOLUTE)
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET cloud-media-client POST_BUILD
        COMMAND "${_qt_bin}/windeployqt.exe" --release --no-translations
                --no-compiler-runtime $<TARGET_FILE:cloud-media-client>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mingw_bin}/libgcc_s_dw2-1.dll" $<TARGET_FILE_DIR:cloud-media-client>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mingw_bin}/libstdc++-6.dll" $<TARGET_FILE_DIR:cloud-media-client>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mingw_bin}/libwinpthread-1.dll" $<TARGET_FILE_DIR:cloud-media-client>
        COMMENT "Deploying Qt + compiler runtime DLLs"
    )
endif()

# 播放器运行期 DLL：源文件放在 client/player/deps/dll（.gitignore 忽略），
# 由 scripts/sync_player_dlls.sh 从 legacy 复制；这里只做"有就拷到 exe 旁"。
file(GLOB CM_PLAYER_DLLS "${CMAKE_CURRENT_SOURCE_DIR}/player/deps/dll/*.dll")
if(CM_PLAYER_DLLS)
    add_custom_command(TARGET cloud-media-client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CM_PLAYER_DLLS}
                "$<TARGET_FILE_DIR:cloud-media-client>"
    )
endif()

# 客户端配置样例一并放到 exe 旁边，开箱即用（改仓库根的 client.conf 为准）。
if(EXISTS "${CMAKE_SOURCE_DIR}/client.conf")
    add_custom_command(TARGET cloud-media-client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/client.conf"
                "$<TARGET_FILE_DIR:cloud-media-client>"
    )
endif()

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
