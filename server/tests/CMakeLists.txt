# cmake 3.5 没有 gtest_discover_tests，统一用 add_test 显式注册。
# 集成测试只在 ENABLE_DB_INTEGRATION_TESTS=ON 时注册；
# 是否真正执行由 CM_TEST_MYSQL / CM_TEST_REDIS 环境变量决定（未设置时用例 GTEST_SKIP）。

add_executable(test_mysql_pool
    storage/test_mysql_pool.cpp
)
target_link_libraries(test_mysql_pool PRIVATE
    cm_storage
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(test_mysql_pool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME mysql_pool_unit
    COMMAND test_mysql_pool --gtest_filter=-*Integration*
)

if(ENABLE_DB_INTEGRATION_TESTS)
    add_test(NAME mysql_pool_integration
        COMMAND test_mysql_pool --gtest_filter=*Integration*
    )
endif()

add_executable(test_redis_pool
    storage/test_redis_pool.cpp
)
target_link_libraries(test_redis_pool PRIVATE
    cm_cache
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(test_redis_pool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME redis_pool_unit
    COMMAND test_redis_pool --gtest_filter=-*Integration*
)

if(ENABLE_DB_INTEGRATION_TESTS)
    add_test(NAME redis_pool_integration
        COMMAND test_redis_pool --gtest_filter=*Integration*
    )
endif()
