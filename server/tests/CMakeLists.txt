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

add_executable(test_storage_integration
    storage/test_storage_integration.cpp
)
target_link_libraries(test_storage_integration PRIVATE
    cm_storage
    cm_cache
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(test_storage_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME storage_integration COMMAND test_storage_integration)

add_executable(test_schema_contract
    storage/test_schema_contract.cpp
)
target_compile_definitions(test_schema_contract PRIVATE
    CM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(test_schema_contract PRIVATE
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(test_schema_contract PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME schema_contract COMMAND test_schema_contract)

add_executable(test_cache_key
    storage/test_cache_key.cpp
)
target_link_libraries(test_cache_key PRIVATE
    cm_cache
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(test_cache_key PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME cache_key COMMAND test_cache_key)

add_executable(test_schema_v2_2
    storage/test_schema_v2_2.cpp
)
target_link_libraries(test_schema_v2_2 PRIVATE
    cm_storage
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(test_schema_v2_2 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME schema_v2_2 COMMAND test_schema_v2_2)

# DAO 测试：无 CM_TEST_MYSQL=1 时用例内部 GTEST_SKIP，ctest 仍视为通过。
# DAO-5 会把这里改为 add_subdirectory(logic) + 统一的 logic_unit target。
add_executable(test_user_dao
    logic/test_user_dao.cpp
)
target_link_libraries(test_user_dao PRIVATE
    cm_storage
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(test_user_dao PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME user_dao COMMAND test_user_dao)

add_executable(test_file_dao
    logic/test_file_dao.cpp
)
target_compile_definitions(test_file_dao PRIVATE
    CM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(test_file_dao PRIVATE
    cm_storage
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(test_file_dao PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME file_dao COMMAND test_file_dao)
