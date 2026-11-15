#!/usr/bin/env bash
# 边界场景验证（阶段 4 验收口径收尾：队列满 / DB 断连 / 磁盘满）
#
#   E1 队列满   → 提交返回 503（queue_capacity=2，worker 被长任务占住）
#   E2 DB 断连  → 下载不依赖 DB 继续完成（文件校验和一致）；
#                 DB 行停留在 downloading（脏状态）→ 重启后 reconcile 收敛回 success
#   E3 磁盘满   → pwrite/ftruncate ENOSPC → 任务快速 failed，fail_reason 记录磁盘错误
#
# 用法: sudo bash scripts/edge_test.sh [build_dir]
#   需要 root：E3 要挂 8MB tmpfs 模拟磁盘满，E2 要控制 mysqld
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
SRV="$BUILD/download_server"
FILE_PORT=18082
SRV_PORT=19092
RATE_MB=2                      # 聚合速率 = 4 分片 × 2MB/s = 8MB/s，100MB ≈ 12s

WORK_DIR="$(mktemp -d /tmp/dlsvc_edge.XXXXXX)"
STORAGE="$WORK_DIR/storage"
LOG="$WORK_DIR/server.log"
DISK_FULL_DIR="$WORK_DIR/diskfull"   # 8MB tmpfs 挂载点（E3）
SRC="$WORK_DIR/big.bin"
SRC_SHA=""

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  [PASS] $1"; }
bad() { FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }
SRV_PID=""

start_server() { # $1=storage_root $2=worker_count $3=queue_capacity
    local conf="$WORK_DIR/server.conf"
    cat > "$conf" <<EOF
listen_port = $SRV_PORT
storage_root = $1
worker_count = $2
queue_capacity = $3
max_active_per_user = 1000
max_segments_per_task = 4
flush_interval_ms = 1000
mysql_host = 127.0.0.1
mysql_user = dlsvc
mysql_password = dlsvc123
mysql_db = downloader
EOF
    "$SRV" -c "$conf" >>"$LOG" 2>&1 &
    SRV_PID=$!
    for i in $(seq 1 50); do
        curl -sf "http://127.0.0.1:$SRV_PORT/healthz" >/dev/null 2>&1 && return 0
        sleep 0.2
    done
    return 1
}

stop_server() {
    [[ -n "${SRV_PID:-}" ]] && kill "$SRV_PID" 2>/dev/null && wait "$SRV_PID" 2>/dev/null
    SRV_PID=""
}

wait_mysql_up() {
    for i in $(seq 1 50); do
        mysql -h127.0.0.1 -udlsvc -pdlsvc123 downloader -e 'SELECT 1;' >/dev/null 2>&1 && return 0
        sleep 0.4
    done
    return 1
}

cleanup() {
    stop_server
    [[ -n "${FS_PID:-}" ]] && kill "$FS_PID" 2>/dev/null
    mountpoint -q "$DISK_FULL_DIR" 2>/dev/null && umount "$DISK_FULL_DIR" 2>/dev/null
    service mysql start >/dev/null 2>&1   # E2 停过 mysqld，保证退出时恢复
    if [[ "$FAIL" -gt 0 ]]; then
        echo "(edge failed, keep workdir for forensics: $WORK_DIR)"
    else
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

echo "== edge test, workdir=$WORK_DIR =="

if [[ "$(id -u)" != "0" ]]; then
    echo "请用 sudo/root 运行：E3 需要挂 tmpfs 模拟磁盘满，E2 需要控制 mysqld"
    exit 1
fi
[[ -x "$SRV" ]] || { echo "server binary not found at $SRV"; exit 1; }
command -v curl >/dev/null && command -v python3 >/dev/null || { echo "curl/python3 required"; exit 1; }

service mysql start >/dev/null 2>&1
wait_mysql_up && ok "mysql up" || { echo "mysql not reachable"; exit 1; }

dd if=/dev/urandom of="$SRC" bs=1M count=100 2>/dev/null
SRC_SHA=$(sha1sum "$SRC" | awk '{print $1}')
DL_RATE=$((RATE_MB*1024*1024)) python3 "$ROOT/scripts/range_server.py" "$FILE_PORT" "$WORK_DIR" &
FS_PID=$!
mkdir -p "$STORAGE"
API="http://127.0.0.1:$SRV_PORT"
URL="http://127.0.0.1:$FILE_PORT/big.bin"

# ================= E1: 队列满 → 503 =================
echo "-- E1: queue full (worker_count=1, queue_capacity=2, 任务跑 ~12s 占住 worker)"
start_server "$STORAGE" 1 2 || { echo "server start failed"; exit 1; }

CODES=""
for i in 1 2 3 4; do
    CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$API/api/task/submit" \
        -d "{\"url\":\"$URL\",\"save_path\":\"e1_$i.bin\",\"user_id\":\"e1\"}")
    CODES="$CODES $CODE"
done
echo "  submit codes:$CODES"
N503=$(echo "$CODES" | grep -o '503' | wc -l)
N200=$(echo "$CODES" | grep -o '200' | wc -l)
[[ "$N503" -ge 1 && "$N200" -ge 2 ]] && ok "queue full rejected with 503" || bad "expect >=1 x503 and >=2 x200, got:$CODES"

# 收尾：把 e1 的非终态任务全部取消并等到位——否则它们会被下一个服务实例
# 的 reconcile 接走，污染 E2 的测试窗口（第一次跑就栽在这上面）
for i in $(seq 1 20); do
    LEFT=$(mysql -h127.0.0.1 -udlsvc -pdlsvc123 downloader -N -e \
        "SELECT COUNT(*) FROM task WHERE user_id='e1' AND status IN ('pending','downloading');" 2>/dev/null)
    [[ "$LEFT" == "0" ]] && break
    for ID in $(mysql -h127.0.0.1 -udlsvc -pdlsvc123 downloader -N -e \
        "SELECT id FROM task WHERE user_id='e1' AND status IN ('pending','downloading');" 2>/dev/null); do
        curl -s -X POST "$API/api/task/$ID/cancel" >/dev/null
    done
    sleep 0.5
done
LEFT=$(mysql -h127.0.0.1 -udlsvc -pdlsvc123 downloader -N -e \
    "SELECT COUNT(*) FROM task WHERE user_id='e1' AND status IN ('pending','downloading');" 2>/dev/null)
[[ "$LEFT" == "0" ]] && ok "e1 leftovers all terminal" || bad "e1 leftovers still active: $LEFT"
stop_server

# ================= E2: DB 断连 → 下载不中断，重启后对账收敛 =================
echo "-- E2: mysql down mid-download (下载层不依赖 DB；脏状态靠重启对账收敛)"
start_server "$STORAGE" 2 64 || { echo "server start failed"; exit 1; }
E2_ID=$(curl -sf -X POST "$API/api/task/submit" \
    -d "{\"url\":\"$URL\",\"save_path\":\"e2.bin\",\"user_id\":\"e2\"}" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["task_id"])')
[[ -n "$E2_ID" ]] && ok "e2 submitted id=$E2_ID" || bad "e2 submit failed"
sleep 3                                  # 下载进行到 ~25MB
service mysql stop >/dev/null 2>&1
sleep 2                                  # 等 mysqld 完全停掉

# 文件完成判定：rename 原子发布后 $STORAGE/e2.bin 才会出现
COMPLETE="no"
for i in $(seq 1 120); do
    [[ -f "$STORAGE/e2.bin" ]] && { COMPLETE="yes"; break; }
    sleep 1
done
[[ "$COMPLETE" == "yes" ]] && ok "download finished while mysql was down" || bad "download did not finish"
E2_SHA=$(sha1sum "$STORAGE/e2.bin" 2>/dev/null | awk '{print $1}')
[[ "$E2_SHA" == "$SRC_SHA" ]] && ok "checksum ok without db" || bad "checksum mismatch: $E2_SHA"
grep -q "flusher: task .* failed, will retry" "$LOG" \
    && ok "flusher logged retry-on-db-down warnings" || bad "no flusher warnings found"
grep -q "task $E2_ID -> success" "$LOG" \
    && ok "runner reached success independently of db" || bad "runner did not reach success"

stop_server
service mysql start >/dev/null 2>&1
wait_mysql_up && ok "mysql back up" || bad "mysql failed to come back"
STALE=$(mysql -h127.0.0.1 -udlsvc -pdlsvc123 downloader -N -e \
    "SELECT status FROM task WHERE id=$E2_ID;" 2>/dev/null)
[[ "$STALE" == "downloading" ]] && ok "db row stuck at 'downloading' (为什么需要 reconcile)" \
    || bad "stale status=$STALE (expect downloading)"

# 重启服务 → D5 对账 → 任务重新排队 → tmp 已被 rename 走 → 全量重下 → success
start_server "$STORAGE" 2 64 || { echo "server restart failed"; exit 1; }
ST=""
for i in $(seq 1 180); do
    ST=$(curl -sf "$API/api/task/$E2_ID" | python3 -c 'import json,sys; print(json.load(sys.stdin)["status"])' 2>/dev/null)
    [[ "$ST" == "success" || "$ST" == "failed" ]] && break
    sleep 1
done
[[ "$ST" == "success" ]] && ok "reconcile converged: task back to success" || bad "after reconcile status=$ST"
stop_server

# ================= E3: 磁盘满 → 快速失败并记录原因 =================
echo "-- E3: disk full (8MB tmpfs, 下载 100MB → ENOSPC)"
mkdir -p "$DISK_FULL_DIR"
mount -t tmpfs -o size=8m tmpfs "$DISK_FULL_DIR" \
    && ok "8MB tmpfs mounted" || { bad "tmpfs mount failed"; exit 1; }
start_server "$DISK_FULL_DIR" 2 64 || { echo "server start failed"; exit 1; }
E3_ID=$(curl -sf -X POST "$API/api/task/submit" \
    -d "{\"url\":\"$URL\",\"save_path\":\"e3.bin\",\"user_id\":\"e3\"}" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["task_id"])')
ST=""; REASON=""
for i in $(seq 1 60); do
    J=$(curl -sf "$API/api/task/$E3_ID" 2>/dev/null)
    ST=$(echo "$J" | python3 -c 'import json,sys; print(json.load(sys.stdin)["status"])' 2>/dev/null)
    REASON=$(echo "$J" | python3 -c 'import json,sys; print(json.load(sys.stdin)["fail_reason"])' 2>/dev/null)
    [[ "$ST" == "failed" ]] && break
    sleep 1
done
if [[ "$ST" == "failed" && "$REASON" =~ (space|ENOSPC|pwrite|ftruncate) ]]; then
    ok "task failed fast with disk reason: $REASON"
else
    bad "status=$ST reason=$REASON"
fi
stop_server

echo ""
echo "== edge result: PASS=$PASS FAIL=$FAIL =="
[[ "$FAIL" == "0" ]]
