cmake_minimum_required(VERSION 3.5)
project(cloud_media VERSION 0.1.0 LANGUAGES CXX)

# Threads::Threads 是 IMPORTED target，必须在顶层 find_package 才能让所有子目录可见
find_package(Threads REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(options)
include(compiler_flags)
include(thirdparty)

if(BUILD_TESTS)
    enable_testing()
    include(tests)
endif()

add_subdirectory(common)

if(BUILD_SERVER)
    add_subdirectory(server)
endif()

if(BUILD_CLIENT)
    add_subdirectory(client)
endif()

if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

message(STATUS "cloud-media ${PROJECT_VERSION}")
message(STATUS "  BUILD_SERVER = ${BUILD_SERVER}")
message(STATUS "  BUILD_CLIENT = ${BUILD_CLIENT}")
message(STATUS "  BUILD_TESTS  = ${BUILD_TESTS}")
message(STATUS "  BUILD_STORAGE = ${BUILD_STORAGE}")
message(STATUS "  DB_INTEGRATION = ${ENABLE_DB_INTEGRATION_TESTS}")
