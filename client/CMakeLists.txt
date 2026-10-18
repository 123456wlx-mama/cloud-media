add_executable(cloud-media-client
    app/main.cpp
)
target_link_libraries(cloud-media-client PRIVATE cm_protocol)
set_target_properties(cloud-media-client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
