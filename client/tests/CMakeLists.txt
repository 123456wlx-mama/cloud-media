# 客户端单测（不依赖服务端）：帧重组 / 传输状态机 / SQLite 任务库。
# 注意：32 位 exe 需要 32 位 Qt/MinGW DLL，ctest 的 PATH 在下面统一夹带。

add_executable(client_netapi_tests
    test_frame_assembler.cpp
    ../netapi/FrameAssembler.cpp
)
target_include_directories(client_netapi_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(client_netapi_tests PRIVATE
    cm_protocol
    cm_compile_options
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(client_netapi_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME client_netapi_tests COMMAND client_netapi_tests)

add_executable(client_transfer_tests
    test_transfer_queue.cpp
    ../transfer/TransferQueue.cpp
    ../sqlapi/TaskStore.cpp
    ../netapi/NetClient.cpp
    ../netapi/NetWorker.cpp
    ../netapi/FrameAssembler.cpp
)
target_include_directories(client_transfer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(client_transfer_tests PRIVATE
    cm_protocol
    cm_common
    cm_compile_options
    GTest::gtest
    Qt5::Core
    Qt5::Network
    Qt5::Sql
)
set_target_properties(client_transfer_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME client_transfer_tests COMMAND client_transfer_tests)

add_executable(client_sqlapi_tests
    test_task_store.cpp
    ../sqlapi/TaskStore.cpp
)
target_include_directories(client_sqlapi_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(client_sqlapi_tests PRIVATE
    cm_compile_options
    GTest::gtest
    Qt5::Core
    Qt5::Sql
)
set_target_properties(client_sqlapi_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME client_sqlapi_tests COMMAND client_sqlapi_tests)

# ctest 继承的 PATH 里可能有 64 位 MinGW（cmake 自带那套），
# 32 位 exe 会加载到 64 位 libstdc++-6.dll 直接 0xc000007b。
get_filename_component(_cm_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(_cm_qt_bin "${CMAKE_PREFIX_PATH}")
if(NOT _cm_qt_bin)
    get_target_property(_cm_qt_core Qt5::Core LOCATION)
    get_filename_component(_cm_qt_bin "${_cm_qt_core}" DIRECTORY)
endif()
# 注意：ENVIRONMENT 的值是 CMake 列表，分隔符 ; 必须转义，否则第二条会被吞掉。
string(REPLACE ";" "\\;" _cm_test_path "${_cm_mingw_bin};${_cm_qt_bin}/bin")
set_tests_properties(client_netapi_tests client_transfer_tests client_sqlapi_tests
    PROPERTIES ENVIRONMENT "PATH=${_cm_test_path}"
)
