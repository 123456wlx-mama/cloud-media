# DAO 集成测试统一 target（cmake 3.5 语法，不用 gtest_discover_tests）。
# 五个 DAO 测试源编进同一个可执行，避免各自复制一份 pool 创建与清理代码。

add_executable(cm_logic_tests
    dao_fixture.cpp
    test_dao_fixture.cpp
    test_user_dao.cpp
    test_file_dao.cpp
    test_transfer_dao.cpp
    test_share_dao.cpp
    test_play_dao.cpp
    test_upload_init.cpp
    test_upload_chunk.cpp
    test_continue_upload.cpp
    test_upload_complete.cpp
)
# test_file_dao 的 FileDaoNoOffsetSql 要扫描源码。
target_compile_definitions(cm_logic_tests PRIVATE
    CM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(cm_logic_tests PRIVATE
    cm_logic
    cm_storage
    cm_cache
    net
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(cm_logic_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# 一次跑完所有 DAO 用例。
add_test(NAME logic_unit COMMAND cm_logic_tests)

# 保留既有短名，确保 ctest -R user_dao 之类的命令仍然可用。
add_test(NAME user_dao COMMAND cm_logic_tests --gtest_filter=UserDaoTest.*)
add_test(NAME file_dao COMMAND cm_logic_tests --gtest_filter=FileDaoTest.*)
add_test(NAME transfer_dao COMMAND cm_logic_tests --gtest_filter=TransferDaoTest.*)
add_test(NAME share_dao COMMAND cm_logic_tests --gtest_filter=ShareDaoTest.*)
add_test(NAME play_dao COMMAND cm_logic_tests --gtest_filter=PlayDaoTest.*)
add_test(NAME upload_init COMMAND cm_logic_tests --gtest_filter=UploadInit*)
add_test(NAME upload_chunk COMMAND cm_logic_tests --gtest_filter=UploadChunk*)
add_test(NAME continue_upload COMMAND cm_logic_tests --gtest_filter=ContinueUpload*)
add_test(NAME upload_complete COMMAND cm_logic_tests --gtest_filter=UploadComplete*)
