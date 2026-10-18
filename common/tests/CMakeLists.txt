add_executable(cm_common_tests
    test_logger.cpp
    test_buffer.cpp
    test_crc32.cpp
    test_ini_config.cpp
    test_server_config.cpp
    test_error_code.cpp
    ../protocol/tests/test_serialize.cpp
    ../protocol/tests/test_codec.cpp
)
target_link_libraries(cm_common_tests PRIVATE cm_common cm_protocol GTest::gtest GTest::gtest_main)

# gtest_discover_tests 需要 CMake 3.9+，目标环境 3.5，用 add_test 整体注册
add_test(NAME cm_common_tests COMMAND cm_common_tests)
