# cm_net：Reactor 网络库（epoll ET + EPOLLONESHOT），Linux 专用
add_library(net STATIC
    Channel.cpp
    Poller.cpp
    EpollPoller.cpp
    EventLoop.cpp
    EventLoopThread.cpp
    Socket.cpp
    Acceptor.cpp
    TcpConnection.cpp
    TcpServer.cpp
    TimerQueue.cpp
    SignalHandler.cpp
)

target_include_directories(net PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(net PUBLIC cm_common)

if(WIN32)
    # Windows 下 epoll 为编译占位实现，仅用于 CI 编译验证
    target_compile_definitions(net PUBLIC NET_PLATFORM_WINDOWS=1)
    target_link_libraries(net PUBLIC ws2_32)
else()
    target_link_libraries(net PUBLIC Threads::Threads)
endif()

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
