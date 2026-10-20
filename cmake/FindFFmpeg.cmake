# 手写 FFmpeg 查找模块（MinGW 导入库 libav*.dll.a）。
# FFMPEG_ROOT 未指定时用仓库内的 deps（见 docs/plans/phase-8.md 环境段）。
if(NOT FFMPEG_ROOT)
    set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/client/player/deps/ffmpeg")
endif()

find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
    PATHS "${FFMPEG_ROOT}/include"
    NO_DEFAULT_PATH)

set(FFMPEG_LIBRARIES "")
foreach(_cm_ff_name avformat avcodec avutil swscale swresample)
    string(TOUPPER "${_cm_ff_name}" _cm_ff_upper)
    find_library(FFMPEG_${_cm_ff_upper}_LIBRARY
        NAMES ${_cm_ff_name}
        PATHS "${FFMPEG_ROOT}/lib"
        NO_DEFAULT_PATH)
    if(NOT FFMPEG_${_cm_ff_upper}_LIBRARY)
        message(FATAL_ERROR "FFmpeg: 找不到 lib${_cm_ff_name}（FFMPEG_ROOT=${FFMPEG_ROOT}）")
    endif()
    list(APPEND FFMPEG_LIBRARIES ${FFMPEG_${_cm_ff_upper}_LIBRARY})
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg DEFAULT_MSG FFMPEG_INCLUDE_DIR FFMPEG_LIBRARIES)
