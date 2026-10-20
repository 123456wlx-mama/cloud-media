# 手写 SDL2 查找模块（24 位工具链用 32 位 SDL2.lib）。
if(NOT SDL2_ROOT)
    set(SDL2_ROOT "${CMAKE_SOURCE_DIR}/client/player/deps/sdl2")
endif()

find_path(SDL2_INCLUDE_DIR SDL.h
    PATHS "${SDL2_ROOT}/include"
    NO_DEFAULT_PATH)
find_library(SDL2_LIBRARY
    NAMES SDL2 SDL2.lib
    PATHS "${SDL2_ROOT}/lib"
    NO_DEFAULT_PATH)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SDL2 DEFAULT_MSG SDL2_INCLUDE_DIR SDL2_LIBRARY)
