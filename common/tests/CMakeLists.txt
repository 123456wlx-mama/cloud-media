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

add_executable(test_md5
    test_md5.cpp
)
target_link_libraries(test_md5 PRIVATE cm_common GTest::gtest GTest::gtest_main)
set_target_properties(test_md5 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME md5 COMMAND test_md5)

add_executable(test_crc32_combine
    test_crc32_combine.cpp
)
target_link_libraries(test_crc32_combine PRIVATE cm_common GTest::gtest GTest::gtest_main)
set_target_properties(test_crc32_combine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME crc32_combine COMMAND test_crc32_combine)
