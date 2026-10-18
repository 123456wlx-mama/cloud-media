add_executable(cloud-media-server
    app/main.cpp
)
target_link_libraries(cloud-media-server PRIVATE cm_protocol)
set_target_properties(cloud-media-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
