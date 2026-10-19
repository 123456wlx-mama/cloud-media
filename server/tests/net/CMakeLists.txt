# 路由注册表单测：不依赖 MySQL/Redis，只验证 type → handler 与错误映射。
# cmake 3.5 语法，add_test 显式注册。

add_executable(test_handler_registry
    test_handler_registry.cpp
)
target_link_libraries(test_handler_registry PRIVATE
    net
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(test_handler_registry PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME handler_registry COMMAND test_handler_registry)
