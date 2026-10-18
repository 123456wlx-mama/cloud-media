find_package(Threads REQUIRED)

add_library(cm_common STATIC
    base/version.cpp
    base/error_code.cpp
    base/time_util.cpp
    base/buffer.cpp
    base/crc32.cpp
    base/md5.cpp
    base/thread_pool.cpp
    config/ini_config.cpp
    config/server_config.cpp
    log/logger.cpp
)
target_include_directories(cm_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(cm_common PUBLIC cm_compile_options Threads::Threads)

add_library(cm_protocol STATIC
    protocol/version.cpp
    protocol/serialize.cpp
    protocol/codec.cpp
)
target_include_directories(cm_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol/include
)
target_link_libraries(cm_protocol PUBLIC cm_common)

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
