#!/usr/bin/env bash
# C++11 红线自检（路线图 9.4 + 裸 new）。
# 只扫描 common/ 与 server/；legacy/、build*/、thirdparty/ 不在范围。
#
# 已知豁免（C API 兼容，非项目结构体）：
#   memset(&MYSQL_BIND / struct tm / sockaddr / addrinfo / sigaction) 必须零初始化。
# 字符串与注释里的 NULL 不算违规（脚本会先剥离字符串/行注释再匹配）。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

FILES=$(find common server -type f \( -name '*.cpp' -o -name '*.h' \) | sort)
STATUS=0

section() { printf '\n== %s ==\n' "$1"; }
report() {
    if [ -n "$1" ]; then
        printf '%s\n' "$1"
        STATUS=1
    else
        echo "clean"
    fi
}

section "裸 delete（排除 = delete 与字符串）"
HITS=$(grep -nH '\bdelete\b' $FILES 2>/dev/null | grep -v '= delete' | grep -v '"' || true)
report "$HITS"

section "volatile"
HITS=$(grep -nH '\bvolatile\b' $FILES 2>/dev/null || true)
report "$HITS"

section "memset(&C 结构体)（MYSQL_BIND/tm/sockaddr/addrinfo/sigaction 豁免）"
# 豁免：MYSQL_BIND 与 C 结构体（tm/sockaddr/addrinfo/sigaction）的零初始化。
HITS=$(grep -nH 'memset(&' $FILES 2>/dev/null \
    | grep -Ev '&m_paramBinds\[|&bind,|&tm\b|&tm_value|&target|&addr|&sa,|&action|&hints' \
    || true)
report "$HITS"

section "NULL（已剥离字符串与行注释）"
if command -v perl >/dev/null 2>&1; then
    HITS=$(perl -ne 's{"(?:\\.|[^"\\])*"}{}g; s{//.*$}{}; print "$ARGV:$.:$_" if /\bNULL\b/; close ARGV if eof;' $FILES)
else
    HITS=$(grep -nH '\bNULL\b' $FILES 2>/dev/null || true)
fi
report "$HITS"

section "C++14/17 越界特性"
HITS=$(grep -nH 'std::optional\|string_view\|std::filesystem\|std::make_unique' $FILES 2>/dev/null | grep -v '^.*//' || true)
report "$HITS"

section "裸 new（排除 unique_ptr/shared_ptr/reset/make_shared/makeUnique 包装）"
HITS=$(awk '
    FNR == 1 { prev = "" }
    {
        line = $0
        if (line ~ /^[[:space:]]*\/\//) { prev = line; next }
        allowed = "unique_ptr|shared_ptr|reset[(]|make_shared|makeUnique"
        if (line ~ /[[:space:]]new[[:space:]]/ && \
            line !~ allowed && prev !~ allowed) {
            print FILENAME ":" FNR ":" line
        }
        prev = line
    }
' $FILES) || HITS="awk failed"
report "$HITS"

printf '\n'
if [ "$STATUS" -eq 0 ]; then
    echo "ALL CLEAN"
else
    echo "VIOLATIONS FOUND"
fi
exit "$STATUS"
