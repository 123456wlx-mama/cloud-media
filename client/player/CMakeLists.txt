# 播放器层：唯一允许 include libav*/SDL 的地方（依赖红线见 docs/specs/01-架构设计.md）。
find_package(FFmpeg REQUIRED)
find_package(SDL2 REQUIRED)

add_library(cm_player_core STATIC
    PlayerCore.cpp
    PlayerCore.h
    PacketQueue.cpp
    PacketQueue.h
    GrowingFileIO.cpp
    GrowingFileIO.h
)

# PlayerCore 有 Q_OBJECT：这个 target 在 add_subdirectory 里，AUTOMOC 需要显式打开
set_target_properties(cm_player_core PROPERTIES AUTOMOC ON)

target_include_directories(cm_player_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
# 第三方头文件按 SYSTEM 处理：不把 FFmpeg/SDL 的告警算到我们头上
target_include_directories(cm_player_core SYSTEM PRIVATE
    ${FFMPEG_INCLUDE_DIR}
    ${SDL2_INCLUDE_DIR}
)

target_link_libraries(cm_player_core PUBLIC
    Qt5::Core
    Qt5::Gui
    ${FFMPEG_LIBRARIES}
    ${SDL2_LIBRARY}
)
target_link_libraries(cm_player_core PRIVATE cm_compile_options)
