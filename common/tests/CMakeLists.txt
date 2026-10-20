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

# 业务消息编解码（06 第 3/4 节）：独立 target，短名 upload_messages。
add_executable(test_business_messages
    ../protocol/tests/test_business_messages.cpp
)
target_link_libraries(test_business_messages PRIVATE
    cm_protocol
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(test_business_messages PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME upload_messages COMMAND test_business_messages)

# Phase 6 T6-6：Codec/Buffer Fuzz 驱动（独立可执行，非 gtest）。
# 配合 -DENABLE_ASAN=ON 手动运行（ASan 旗标经 cm_compile_options 传递）。
# 不注册进 ctest：长跑（默认 1M 迭代）+ ASan 专用，避免拖慢常规 CI。
add_executable(cm_fuzz_driver
    ../protocol/tests/fuzz_codec_driver.cpp
)
target_link_libraries(cm_fuzz_driver PRIVATE cm_protocol)
set_target_properties(cm_fuzz_driver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
