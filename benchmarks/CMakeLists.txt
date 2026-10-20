add_executable(cm_log_bench
    log_bench.cpp
)
target_link_libraries(cm_log_bench PRIVATE cm_common)
set_target_properties(cm_log_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_executable(bencher bencher.cpp)
target_link_libraries(bencher PRIVATE cm_protocol cm_common)
if(NOT WIN32)
    target_link_libraries(bencher PRIVATE Threads::Threads)
endif()
set_target_properties(bencher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_executable(bench_conn bench_conn.cpp)
if(NOT WIN32)
    target_link_libraries(bench_conn PRIVATE Threads::Threads)
endif()
set_target_properties(bench_conn PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(BUILD_STORAGE AND BUILD_SERVER AND TARGET cm_storage AND TARGET cm_cache)
    add_executable(bench_db bench_db.cpp)
    target_link_libraries(bench_db PRIVATE cm_storage cm_cache)
    set_target_properties(bench_db PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()
