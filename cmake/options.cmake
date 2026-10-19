if(WIN32)
    set(_cm_default_server OFF)
    set(_cm_default_client ON)
else()
    set(_cm_default_server ON)
    set(_cm_default_client OFF)
endif()

option(BUILD_SERVER "Build Linux server (epoll)" ${_cm_default_server})
option(BUILD_CLIENT "Build client (Qt in Phase 8)" ${_cm_default_client})
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_ASAN "Enable ASan/UBSan in Debug" OFF)
option(ENABLE_TSAN "Enable TSan in Debug" OFF)
option(ENABLE_COVERAGE "Enable gcov coverage" OFF)
option(USE_BUNDLED_THIRDPARTY "Prefer thirdparty/ over system libraries" ${WIN32})
option(BUILD_BENCHMARKS "Build benchmarks" ON)
# H1 门槛已 PASS（docs/benchmark/phase5_zero_copy_gate.md），默认走零拷贝；
# 需要排障时用 -DCM_DOWNLOAD_ZEROCOPY=OFF 回退到缓冲路径。
option(CM_DOWNLOAD_ZEROCOPY "Use sendfile zero-copy block path" ON)

if(BUILD_SERVER AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "BUILD_SERVER is Linux-only (epoll). Use WSL or a Linux machine.")
endif()

if(WIN32)
    set(_cm_default_storage OFF)
else()
    set(_cm_default_storage ON)
endif()

option(BUILD_STORAGE "Build MySQL storage layer (Linux only)" ${_cm_default_storage})
option(ENABLE_DB_INTEGRATION_TESTS "Run storage tests against a real MySQL" OFF)

if(BUILD_STORAGE AND WIN32)
    message(FATAL_ERROR
        "BUILD_STORAGE is Linux-only: libmysqlclient is not bundled on Windows.")
endif()

if(BUILD_SERVER AND NOT BUILD_STORAGE)
    message(WARNING
        "BUILD_SERVER=ON with BUILD_STORAGE=OFF: server will run without persistence.")
endif()

if(BUILD_STORAGE AND NOT BUILD_SERVER)
    message(WARNING
        "BUILD_STORAGE=ON requires BUILD_SERVER=ON; cm_storage will not be built.")
endif()

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN cannot both be ON.")
endif()
