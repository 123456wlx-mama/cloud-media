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
        db/transfer_dao.cpp
        db/share_dao.cpp
        db/play_dao.cpp
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

    # 业务层：只经 DAO 与 CacheStore 访问存储，不出现 MySQL/Redis C API。
    add_library(cm_logic STATIC
        logic/connection_decoder.cpp
        logic/upload_service.cpp
        logic/upload_chunk_handler.cpp
        logic/upload_fs.cpp
        logic/expiry_job.cpp
        logic/download_service.cpp
        logic/block_crc.cpp
        logic/download_ticket.cpp
        logic/block_sender.cpp
        logic/block_sender_factory.cpp
        logic/sendfile_block_sender.cpp
        logic/rate_limiter.cpp
        logic/download_handlers.cpp
        logic/share_service.cpp
        logic/recycle_service.cpp
        logic/file_service.cpp
        logic/play_service.cpp
    )
    target_include_directories(cm_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cm_logic
        PUBLIC cm_protocol cm_storage cm_cache
        PRIVATE net Threads::Threads)

    # D-0：H1 通过后默认 ON；OFF 仍可构建，下载回退缓冲路径。
    # PUBLIC 让测试能断言 zero_copy 与编译期开关一致。
    if(CM_DOWNLOAD_ZEROCOPY)
        target_compile_definitions(cm_logic PUBLIC CM_DOWNLOAD_ZEROCOPY=1)
    else()
        target_compile_definitions(cm_logic PUBLIC CM_DOWNLOAD_ZEROCOPY=0)
    endif()
endif()

add_executable(cloud-media-server
    app/main.cpp
)
target_link_libraries(cloud-media-server PRIVATE cm_protocol)
if(BUILD_STORAGE)
    # ServerApp 只在有存储层时编译：它依赖 cm_logic/MySQL 启动恢复。
    target_sources(cloud-media-server PRIVATE app/server_app.cpp)
endif()
if(BUILD_STORAGE)
    # main.cpp 用 CM_WITH_STORAGE 区分“有业务层”与“仅骨架”两种构建。
    target_compile_definitions(cloud-media-server PRIVATE CM_WITH_STORAGE=1)
    # net 放在 cm_logic 之后：GNU ld 从左到右解析，依赖方必须先出现。
    target_link_libraries(cloud-media-server PRIVATE
        cm_logic cm_storage cm_cache net)
else()
    target_link_libraries(cloud-media-server PRIVATE net)
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
