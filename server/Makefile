# ============================================================================
#  cloud-media 服务端：纯 g++ 构建，**不依赖 CMake**
#
#  用法（在 server/ 目录下）：
#      make -j4          # 增量编译，产物 ../build-gmake/cloud-media-server
#      make clean        # 清掉产物
#
#  运行：
#      ../build-gmake/cloud-media-server -c <配置.ini>
#
#  依赖（Ubuntu）：g++ / libmysqlclient-dev / libhiredis-dev
#      sudo apt-get install -y g++ libmysqlclient-dev libhiredis-dev
#
#  与 CMake 构建互不影响：CMake 用 build-on/、build-cov/，这里用 build-gmake/。
# ============================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra -Wpedantic

# 头文件：server 各子目录用扁平 include（#include "EventLoop.h"），common 用 cm/ 前缀
CPPFLAGS += -I. -Inet -Ilogic -Idb -Icache -Iapp \
            -I../common/include -I../common/protocol/include \
            -I/usr/include/mysql

# 与 CMake 的 BUILD_STORAGE / CM_DOWNLOAD_ZEROCOPY 对齐
DEFINES  += -DCM_WITH_STORAGE=1 -DCM_DOWNLOAD_ZEROCOPY=1

LDLIBS   += -lpthread -lmysqlclient -lhiredis

OUT      := ../build-gmake
TARGET   := $(OUT)/cloud-media-server
OBJDIR   := $(OUT)/obj

# 源文件：common 全量（不含 tests）+ server 的 app/net/logic/db/cache（不含 examples）
COMMON_SRCS := $(wildcard ../common/base/*.cpp) \
               $(wildcard ../common/config/*.cpp) \
               $(wildcard ../common/log/*.cpp) \
               $(wildcard ../common/protocol/*.cpp) \
               $(wildcard ../common/protocol/messages/*.cpp)

SERVER_SRCS := $(wildcard app/*.cpp) \
               $(wildcard net/*.cpp) \
               $(wildcard logic/*.cpp) \
               $(wildcard logic/jobs/*.cpp) \
               $(wildcard db/*.cpp) \
               $(wildcard cache/*.cpp)

# 目标文件按源目录分层（common/base/version.cpp 与 common/protocol/version.cpp 同名，必须分开）
COMMON_OBJS := $(patsubst ../common/%.cpp,$(OBJDIR)/common/%.o,$(COMMON_SRCS))
SERVER_OBJS := $(patsubst %.cpp,$(OBJDIR)/server/%.o,$(SERVER_SRCS))
OBJS        := $(COMMON_OBJS) $(SERVER_OBJS)

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDLIBS)
	@echo "==> built $@"

$(OBJDIR)/common/%.o: ../common/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) -c $< -o $@

$(OBJDIR)/server/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) -c $< -o $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
