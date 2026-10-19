add_subdirectory(net)

if(BUILD_TESTS)
    add_subdirectory(logic/tests)
endif()

if(BUILD_STORAGE)
    cm_find_mysql()
    cm_find_hiredis()

    add_library(cm_storage STATIC
        db/mysql_pool.cpp
        db/mysql_conn.cpp
        db/prepared_statement.cpp
        db/user_dao.cpp
        db/file_dao.cpp
    )
    target_include_directories(cm_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cm_storage
        PUBLIC cm_common cm::mysql
        PRIVATE Threads::Threads)

    add_library(cm_cache STATIC
        cache/redis_pool.cpp
        cache/cache_store.cpp
    )
    target_include_directories(cm_cache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cm_cache
        PUBLIC cm_common cm::hiredis
        PRIVATE Threads::Threads)
endif()

add_executable(cloud-media-server
    app/main.cpp
)
target_link_libraries(cloud-media-server PRIVATE cm_protocol)
if(BUILD_STORAGE)
    target_link_libraries(cloud-media-server PRIVATE cm_storage cm_cache)
endif()
set_target_properties(cloud-media-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_executable(echo_server examples/echo_server.cpp)
target_link_libraries(echo_server PRIVATE net)
set_target_properties(echo_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(BUILD_STORAGE AND BUILD_TESTS)
    add_subdirectory(tests)
endif()
