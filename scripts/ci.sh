#!/usr/bin/env bash
# 一键 CI：C++11 红线 + 构建 + 全量 ctest + 覆盖率门槛。
# 密码只从 CM_MYSQL_PASSWORD 读取；不写入任何日志。
set -euo pipefail

: "${CM_MYSQL_PASSWORD:?set CM_MYSQL_PASSWORD}"
: "${CM_TEST_MYSQL_USER:=cm_test}"
: "${CM_TEST_MYSQL_DB:=cloud_media_test}"
: "${CI_JOBS:=4}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${CI_BUILD_DIR:-$ROOT/build-cov}"

echo "== check_cpp11 =="
bash "$ROOT/scripts/check_cpp11.sh"

echo "== configure ($BUILD_DIR) =="
mkdir -p "$BUILD_DIR"
# 每次显式重配：让新增测试/源文件进入构建（cmake 3.5 也支持 -B）。
( cd "$BUILD_DIR" && cmake .. \
    -DBUILD_SERVER=ON \
    -DBUILD_STORAGE=ON \
    -DBUILD_TESTS=ON \
    -DENABLE_COVERAGE=ON \
    -DENABLE_DB_INTEGRATION_TESTS=ON )

echo "== build =="
( cd "$BUILD_DIR" && cmake --build . -- -j"$CI_JOBS" )

if command -v lcov >/dev/null 2>&1; then
    echo "== reset coverage counters =="
    lcov --zerocounters --directory "$BUILD_DIR" >/dev/null 2>&1 || true
fi

echo "== ctest =="
( cd "$BUILD_DIR" && \
  CM_TEST_MYSQL=1 \
  CM_TEST_MYSQL_USER="$CM_TEST_MYSQL_USER" \
  CM_TEST_MYSQL_DB="$CM_TEST_MYSQL_DB" \
  CM_TEST_MYSQL_PASSWORD="$CM_MYSQL_PASSWORD" \
  CM_TEST_REDIS=1 \
  ctest --output-on-failure )

if ! command -v lcov >/dev/null 2>&1; then
    echo "lcov not installed; skip coverage gate" >&2
    exit 0
fi

echo "== coverage =="
cd "$ROOT"
lcov --capture --directory "$BUILD_DIR" \
    --output-file "$BUILD_DIR/coverage.info" >/dev/null
lcov --extract "$BUILD_DIR/coverage.info" "$ROOT/common/*" \
    -o "$BUILD_DIR/common.info" >/dev/null
lcov --extract "$BUILD_DIR/coverage.info" "$ROOT/server/net/*" \
    -o "$BUILD_DIR/net.info" >/dev/null

line_rate() {
    lcov --summary "$1" 2>&1 \
        | awk '/lines\.\.\.\.\.\.:/ { gsub("%", "", $2); print $2; exit }'
}

COMMON_RATE="$(line_rate "$BUILD_DIR/common.info")"
NET_RATE="$(line_rate "$BUILD_DIR/net.info")"
echo "common lines: ${COMMON_RATE}% (DoD >= 80%)"
echo "net lines:    ${NET_RATE}% (DoD >= 60%)"

awk -v v="$COMMON_RATE" 'BEGIN { exit !(v + 0 >= 80) }' || {
    echo "coverage gate failed: common ${COMMON_RATE}% < 80%" >&2
    exit 1
}
awk -v v="$NET_RATE" 'BEGIN { exit !(v + 0 >= 60) }' || {
    echo "coverage gate failed: server/net ${NET_RATE}% < 60%" >&2
    exit 1
}

echo "CI OK"
