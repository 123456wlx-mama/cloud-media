# cmake 3.5 没有 gtest_discover_tests，统一用 add_test 显式注册
# GTest 目标由根 cmake/tests.cmake 通过 add_subdirectory(thirdparty/googletest) 提供

add_executable(test_event_loop test_event_loop.cpp)
target_link_libraries(test_event_loop PRIVATE net GTest::gtest GTest::gtest_main)
set_target_properties(test_event_loop PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME net_event_loop COMMAND test_event_loop)

add_executable(test_thread_pool test_thread_pool.cpp)
target_link_libraries(test_thread_pool PRIVATE net GTest::gtest GTest::gtest_main)
set_target_properties(test_thread_pool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME net_thread_pool COMMAND test_thread_pool)

add_executable(test_tcp_server test_tcp_server.cpp)
target_link_libraries(test_tcp_server PRIVATE net GTest::gtest GTest::gtest_main)
set_target_properties(test_tcp_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME net_tcp_server COMMAND test_tcp_server)

add_executable(test_send_file test_send_file.cpp)
target_link_libraries(test_send_file PRIVATE net GTest::gtest GTest::gtest_main)
set_target_properties(test_send_file PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME send_file COMMAND test_send_file)
