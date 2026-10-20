#!/usr/bin/env bash
# 一键部署 + 冒烟：build → up → 等端口 → 容器内 upload/download + MD5 比对 → DEPLOY_OK
# 失败时打印是哪一步、容器状态与日志 tail。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TEL="${CM_DEMO_TEL:-13900000000}"
PASSWORD="${CM_DEMO_PASSWORD:-demo123456}"
HOST_PORT="${CM_SERVER_PORT:-8004}"
NAME="deploy_smoke_$(date +%s).bin"

COMPOSE="docker compose -f $ROOT/docker-compose.yml"
STEP="init"

say()  { printf '\n== %s\n' "$*"; }
fail() {
    printf '\nDEPLOY_FAIL at step: %s\n' "$STEP" >&2
    printf -- '--- docker compose ps ---\n' >&2
    $COMPOSE ps >&2 || true
    printf -- '--- server logs (tail 60) ---\n' >&2
    $COMPOSE logs --tail=60 server >&2 || true
    printf -- '--- mysql logs (tail 40) ---\n' >&2
    $COMPOSE logs --tail=40 mysql >&2 || true
    exit 1
}
trap 'fail' ERR

STEP="docker compose build"
say "$STEP"
$COMPOSE build

STEP="docker compose up -d"
say "$STEP"
$COMPOSE up -d

STEP="wait server port"
say "$STEP"
PORT_READY=0
for i in $(seq 1 60); do
    if $COMPOSE exec -T server bash -c "exec 3<>/dev/tcp/127.0.0.1/${HOST_PORT}" 2>/dev/null; then
        PORT_READY=1
        break
    fi
    sleep 2
done
if [ "$PORT_READY" -ne 1 ]; then
    echo "server 端口 ${HOST_PORT} 在 120s 内未就绪" >&2
    exit 1
fi
echo "server 已监听 ${HOST_PORT}"

STEP="smoke: upload"
say "$STEP"
$COMPOSE exec -T server bash -c '
set -e
head -c 1048576 /dev/urandom > /tmp/deploy_smoke.bin
md5sum /tmp/deploy_smoke.bin | cut -d" " -f1 > /tmp/deploy_smoke.md5
/app/bin/upload_cli --host 127.0.0.1 --port '"${HOST_PORT}"' \
  --tel '"${TEL}"' --password '"${PASSWORD}"' \
  --file /tmp/deploy_smoke.bin --remote '"${NAME}"'
'

STEP="smoke: locate ufId"
say "$STEP"
UF_ID="$($COMPOSE exec -T server bash -c 'set -e; /app/bin/list_cli --host 127.0.0.1 --port '"${HOST_PORT}"' --tel '"${TEL}"' --password '"${PASSWORD}"' --limit 50' \
    | tr -d '\r' | awk -F'\t' -v name="NAME=${NAME}" 'index($0, name) { gsub("UF_ID=", "", $1); print $1; exit }')"
if [ -z "${UF_ID:-}" ]; then
    echo "列表里找不到 ${NAME}" >&2
    exit 1
fi
echo "UF_ID=${UF_ID}"

STEP="smoke: download + md5"
say "$STEP"
$COMPOSE exec -T server bash -c '
set -e
/app/bin/download_cli --host 127.0.0.1 --port '"${HOST_PORT}"' \
  --tel '"${TEL}"' --password '"${PASSWORD}"' \
  --uf-id '"${UF_ID}"' --out /tmp/deploy_smoke.out
a=$(md5sum /tmp/deploy_smoke.bin | cut -d" " -f1)
b=$(md5sum /tmp/deploy_smoke.out | cut -d" " -f1)
echo "LOCAL_MD5=$a"
echo "REMOTE_MD5=$b"
[ "$a" = "$b" ] || { echo "MD5 不一致" >&2; exit 1; }
'

STEP="done"
say "container status"
$COMPOSE ps

echo ""
echo "DEPLOY_OK  (server: http://127.0.0.1:${HOST_PORT}, demo tel=${TEL})"
