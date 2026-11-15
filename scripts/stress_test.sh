#!/usr/bin/env bash
# 压测：N 个并发任务（同 URL、不同 save_path），统计吞吐 / 排队深度 / 成功率
# 用法: ./scripts/stress_test.sh [tasks] [size_mb] [build_dir]
#   tasks   并发任务数，默认 16
#   size_mb 源文件大小，默认 100
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${3:-$ROOT/build}"
SRV="$BUILD/download_server"
N="${1:-16}"
SIZE_MB="${2:-100}"
FILE_PORT=18081
SRV_PORT=19091

WORK_DIR="$(mktemp -d /tmp/dlsvc_stress.XXXXXX)"
STORAGE="$WORK_DIR/storage"
LOG="$WORK_DIR/server.log"
cleanup() {
    [[ -n "${SRV_PID:-}" ]] && kill "$SRV_PID" 2>/dev/null
    [[ -n "${FS_PID:-}" ]] && kill "$FS_PID" 2>/dev/null
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

echo "== stress: tasks=$N size=${SIZE_MB}MB =="

dd if=/dev/zero of="$WORK_DIR/blob.bin" bs=1M count="$SIZE_MB" 2>/dev/null
python3 "$ROOT/scripts/range_server.py" "$FILE_PORT" "$WORK_DIR" &   # 不限速：压吞吐
FS_PID=$!

mkdir -p "$STORAGE"
cat > "$WORK_DIR/server.conf" <<EOF
listen_port = $SRV_PORT
storage_root = $STORAGE
worker_count = 8
queue_capacity = 256
max_active_per_user = 10000
max_segments_per_task = 4
flush_interval_ms = 2000
mysql_host = 127.0.0.1
mysql_user = dlsvc
mysql_password = dlsvc123
mysql_db = downloader
EOF
"$SRV" -c "$WORK_DIR/server.conf" >"$LOG" 2>&1 &
SRV_PID=$!

API="http://127.0.0.1:$SRV_PORT"
for i in $(seq 1 50); do
    curl -sf "$API/healthz" >/dev/null 2>&1 && break
    sleep 0.2
done

# 唯一 user_id：DB 里可能留着历史压测的 success 行，轮询统计必须只认本轮
STRESS_USER="stress_$(date +%s)"

T0=$(date +%s%N)
MAXQ=0
# 注意：裸 wait 会把前面 & 启动的 range_server/download_server 也等住（永不退出），
# 这里只等提交用的 curl
CURL_PIDS=()
for i in $(seq 1 "$N"); do
    curl -sf -X POST "$API/api/task/submit" \
        -d "{\"url\":\"http://127.0.0.1:$FILE_PORT/blob.bin\",\"save_path\":\"stress_$i.bin\",\"user_id\":\"$STRESS_USER\"}" \
        > "$WORK_DIR/resp_$i.json" &
    CURL_PIDS+=($!)
done
for p in "${CURL_PIDS[@]}"; do wait "$p"; done

# 轮询：全部终态（统计期间记录队列深度峰值）
while true; do
    H=$(curl -sf "$API/healthz")
    Q=$(echo "$H" | python3 -c 'import json,sys; print(json.load(sys.stdin)["queue_size"])')
    A=$(echo "$H" | python3 -c 'import json,sys; print(json.load(sys.stdin)["active_tasks"])')
    (( Q + A > MAXQ )) && MAXQ=$(( Q + A ))
    DONE=$(curl -sf "$API/api/tasks?user_id=$STRESS_USER&limit=500" | python3 -c '
import json,sys
d=json.load(sys.stdin)
print(sum(1 for t in d["tasks"] if t["status"] in ("success","failed","canceled")))')
    echo -ne "\rprogress: $DONE/$N (queue=$Q active=$A)   "
    [[ "$DONE" == "$N" ]] && break
    sleep 1
done
T1=$(date +%s%N)
echo ""

SUC=$(curl -sf "$API/api/tasks?user_id=$STRESS_USER&status=success&limit=500" | python3 -c 'import json,sys; print(json.load(sys.stdin)["count"])')
FAIL=$(curl -sf "$API/api/tasks?user_id=$STRESS_USER&status=failed&limit=500" | python3 -c 'import json,sys; print(json.load(sys.stdin)["count"])')
TOTAL_MB=$(( N * SIZE_MB ))

# 完整性抽查：首尾两个任务的落盘文件必须与源文件逐字节一致
SRC_SHA=$(sha1sum "$WORK_DIR/blob.bin" | awk '{print $1}')
SHA1_OK=$(sha1sum "$STORAGE/stress_1.bin" "$STORAGE/stress_$N.bin" | awk '{print $1}' | grep -c "^$SRC_SHA\$")

echo "== stress result =="
echo "  tasks:       $N (success=$SUC failed=$FAIL)  checksum_ok=$SHA1_OK/$N抽查2个"
echo "  total data:  ${TOTAL_MB} MB"
python3 -c "
wall = ($T1 - $T0) / 1e9
total = $TOTAL_MB
print('  wall time:   %.2fs' % wall)
print('  throughput:  %.1f MB/s (aggregate)' % (total / max(0.001, wall)))"
echo "  max queue+active concurrency: $MAXQ"
[[ "$SUC" == "$N" && "$SHA1_OK" == "2" ]]
