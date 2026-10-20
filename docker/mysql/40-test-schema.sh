#!/bin/bash
# 把同一套 schema 应用到 cloud_media_test（build/test 服务跑集成测试用）。
# 由 mysql 镜像的 docker-entrypoint 在初始化阶段执行。
set -e
mysql -uroot -p"$MYSQL_ROOT_PASSWORD" cloud_media_test < /sql/schema_v2.sql
mysql -uroot -p"$MYSQL_ROOT_PASSWORD" cloud_media_test < /sql/patch_v2_1_index.sql
mysql -uroot -p"$MYSQL_ROOT_PASSWORD" cloud_media_test < /sql/patch_v2_2_phase5.sql
echo "[init] cloud_media_test schema applied"
