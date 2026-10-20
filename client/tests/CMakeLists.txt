# 客户端 netapi 单测（不依赖服务端）：帧重组 / 粘包 / 坏帧。
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

# ctest 继承的 PATH 里可能有 64 位 MinGW（cmake 自带那套），
# 32 位 exe 会加载到 64 位 libstdc++-6.dll 直接 0xc000007b。
# 这里按编译器实际位置拼出 32 位 bin，保证测试无论在什么 shell 下都跑得起来。
get_filename_component(_cm_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
set_tests_properties(client_netapi_tests PROPERTIES
    ENVIRONMENT "PATH=${_cm_mingw_bin}"
)
