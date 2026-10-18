add_subdirectory(${CMAKE_SOURCE_DIR}/thirdparty/googletest EXCLUDE_FROM_ALL)
# include(GoogleTest) 需要 CMake 3.9+，目标环境 3.5，改用 add_test 注册（见各 tests/CMakeLists.txt）
