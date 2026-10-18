add_executable(cm_log_bench
    log_bench.cpp
)
target_link_libraries(cm_log_bench PRIVATE cm_common)
set_target_properties(cm_log_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
