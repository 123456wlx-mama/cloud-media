# cloud-media 服务端镜像（多阶段）：构建链只在 build 层，runtime 只带运行库 + 二进制。
# 用法见 README「Docker Quick Start」。

FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      cmake g++ make \
      libmysqlclient-dev libhiredis-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# BUILD_STORAGE 在 Linux 上默认 ON；BENCHMARKS 关掉可以少编几个二进制。
RUN cmake -S . -B build \
      -DBUILD_SERVER=ON \
      -DBUILD_CLIENT=OFF \
      -DBUILD_TESTS=OFF \
      -DBUILD_BENCHMARKS=OFF \
      -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && ls -1 build/bin

# ---------------------------------------------------------------- runtime
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
# 只装运行期库：构建工具链不进这一层
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libmysqlclient21 libhiredis0.14 ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/bin/ /app/bin/
COPY docker/server.conf /app/server.conf
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh \
 && ldd /app/bin/cloud-media-server | (grep "not found" && exit 1 || true)

# 数据盘：blobs / downloads / logs 都在 /data 下
VOLUME ["/data"]
EXPOSE 8004

ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["/app/bin/cloud-media-server", "-c", "/app/server.conf"]
