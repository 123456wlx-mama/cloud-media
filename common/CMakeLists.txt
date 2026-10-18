find_package(Threads REQUIRED)

add_library(cm_common STATIC
    base/version.cpp
)
target_include_directories(cm_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(cm_common PUBLIC cm_compile_options Threads::Threads)

add_library(cm_protocol STATIC
    protocol/version.cpp
)
target_include_directories(cm_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol/include
)
target_link_libraries(cm_protocol PUBLIC cm_common)
