# cmake 3.5 没有 gtest_discover_tests，统一用 add_test 显式注册。
add_executable(test_bulk_gate
    test_bulk_gate.cpp
    ../connection_decoder.cpp
)
target_include_directories(test_bulk_gate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(test_bulk_gate PRIVATE
    cm_protocol
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(test_bulk_gate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME bulk_gate COMMAND test_bulk_gate)
