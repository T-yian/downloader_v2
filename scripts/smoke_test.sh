#!/usr/bin/env bash
# 冒烟测试：把方案文档阶段 4 的"闭环验收"自动化——
#   提交→排队→下载→落盘→MySQL 终态 / 取消 / 重启对账续跑 / 失败原因 / 用户限流
#
# 前置：build 完成、mysqld 可连、python3 可用
# 用法: ./scripts/smoke_test.sh [build_dir]
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
SRV="$BUILD/download_server"
# 限速是"每连接"的：任务内部分片并发（smoke conf 里 max_segments_per_task=4），
# 聚合速率 = 4 × RATE。设 2MB/s → 聚合 8MB/s，100MB 约 12s——保证 T2 的 cancel
# 和 T4 的 kill -9 都落在下载中段，测试才确定性地测到目标行为
RATE_MB=2
FILE_PORT=18080
SRV_PORT=19090
WORK_DIR="$(mktemp -d /tmp/dlsvc_smoke.XXXXXX)"
STORAGE="$WORK_DIR/storage"
LOG="$WORK_DIR/server.log"

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  [PASS] $1"; }
bad() { FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

cleanup() {
    [[ -n "${SRV_PID:-}" ]] && kill "$SRV_PID" 2>/dev/null
    [[ -n "${FS_PID:-}" ]] && kill "$FS_PID" 2>/dev/null
    if [[ "$FAIL" -gt 0 ]]; then
        echo "(smoke failed, keep workdir for forensics: $WORK_DIR)"
    else
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

echo "== smoke test, workdir=$WORK_DIR =="

# ---------- 0. 前置检查 ----------
if [[ ! -x "$SRV" ]]; then
    echo "server binary not found at $SRV — 先 build（README 构建一节）"
    exit 1
fi
command -v curl >/dev/null || { echo "curl required"; exit 1; }
command -v python3 >/dev/null || { echo "python3 required"; exit 1; }

# ---------- 1. 测试文件 + 本地 Range 文件服务器 ----------
dd if=/dev/urandom of="$WORK_DIR/big.bin" bs=1M count=100 2>/dev/null
SRC_SHA=$(sha1sum "$WORK_DIR/big.bin" | awk '{print $1}')
DL_RATE=$((RATE_MB*1024*1024)) python3 "$ROOT/scripts/range_server.py" "$FILE_PORT" "$WORK_DIR" &
FS_PID=$!

# ---------- 2. 起服务端 ----------
mkdir -p "$STORAGE"
cat > "$WORK_DIR/server.conf" <<EOF
listen_port = $SRV_PORT
storage_root = $STORAGE
worker_count = 2
queue_capacity = 64
max_active_per_user = 2
max_segments_per_task = 4
min_segment_size_mb = 8
flush_interval_ms = 1000
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
    kill -0 "$SRV_PID" 2>/dev/null || { echo "server died at startup:"; cat "$LOG"; exit 1; }
done
curl -sf "$API/healthz" >/dev/null && ok "server healthy" || { echo "server not healthy:"; tail -20 "$LOG"; exit 1; }

# ---------- 3. T1 正常下载：提交 → success → 文件校验和一致 ----------
echo "-- T1: normal download"
TASK_ID=$(curl -sf -X POST "$API/api/task/submit" -d "{\"url\":\"http://127.0.0.1:$FILE_PORT/big.bin\",\"save_path\":\"t1.bin\",\"user_id\":\"u1\"}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["task_id"])')
[[ -n "$TASK_ID" ]] && ok "submit -> task_id=$TASK_ID" || bad "submit"

ST=""
for i in $(seq 1 300); do
    ST=$(curl -sf "$API/api/task/$TASK_ID" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["status"])')
    [[ "$ST" == "success" || "$ST" == "failed" ]] && break
    sleep 1
done
[[ "$ST" == "success" ]] && ok "task reached success" || { bad "task status=$ST (expect success)"; tail -5 "$LOG"; }
OUT_SHA=$(sha1sum "$STORAGE/t1.bin" 2>/dev/null | awk '{print $1}')
[[ "$OUT_SHA" == "$SRC_SHA" ]] && ok "file checksum matches ($OUT_SHA)" || bad "checksum mismatch: $OUT_SHA vs $SRC_SHA"

# ---------- 4. T2 取消：下载中取消 → canceled，断点文件保留 ----------
echo "-- T2: cancel while downloading"
TASK_ID2=$(curl -sf -X POST "$API/api/task/submit" -d "{\"url\":\"http://127.0.0.1:$FILE_PORT/big.bin\",\"save_path\":\"t2.bin\",\"user_id\":\"u2\"}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["task_id"])')
sleep 4   # 让它下载一会儿（限速 8MB/s，必有进度）
curl -sf -X POST "$API/api/task/$TASK_ID2/cancel" >/dev/null && ok "cancel accepted" || bad "cancel"
ST=""
for i in $(seq 1 30); do
    ST=$(curl -sf "$API/api/task/$TASK_ID2" | python3 -c 'import json,sys; print(json.load(sys.stdin)["status"])')
    [[ "$ST" == "canceled" ]] && break
    sleep 1
done
[[ "$ST" == "canceled" ]] && ok "task canceled" || bad "task status=$ST (expect canceled)"
[[ -f "$STORAGE/t2.bin.dltmp" ]] && ok ".dltmp kept (resumable)" || bad ".dltmp missing"

# ---------- 5. T3 断点续传：取消的任务重新提交 → resumed → success 校验 ----------
echo "-- T3: resume canceled task"
RESP=$(curl -sf -X POST "$API/api/task/submit" -d "{\"url\":\"http://127.0.0.1:$FILE_PORT/big.bin\",\"save_path\":\"t2.bin\",\"user_id\":\"u2\"}")
RESUMED=$(echo "$RESP" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("resumed",""))')
[[ "$RESUMED" == "True" ]] && ok "resubmit -> resumed=true" || bad "resumed=$RESP"
T3_ID=$(echo "$RESP" | python3 -c 'import json,sys; print(json.load(sys.stdin)["task_id"])')
ST=""
for i in $(seq 1 300); do
    ST=$(curl -sf "$API/api/task/$T3_ID" | python3 -c 'import json,sys; print(json.load(sys.stdin)["status"])')
    [[ "$ST" == "success" || "$ST" == "failed" ]] && break
    sleep 1
done
OUT_SHA2=$(sha1sum "$STORAGE/t2.bin" 2>/dev/null | awk '{print $1}')
[[ "$ST" == "success" && "$OUT_SHA2" == "$SRC_SHA" ]] && ok "resumed download checksum ok" || bad "resume: status=$ST sha=$OUT_SHA2"

# ---------- 6. T4 重启对账（D5）：下载中 kill -9 → 重启 → 续跑 → success ----------
echo "-- T4: restart reconciliation (kill -9 mid-download)"
LOG_LINES_BEFORE=$(wc -l < "$LOG")
TASK_ID4=$(curl -sf -X POST "$API/api/task/submit" -d "{\"url\":\"http://127.0.0.1:$FILE_PORT/big.bin\",\"save_path\":\"t4.bin\",\"user_id\":\"u4\"}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["task_id"])')
sleep 5
kill -9 "$SRV_PID"; wait "$SRV_PID" 2>/dev/null
"$SRV" -c "$WORK_DIR/server.conf" >>"$LOG" 2>&1 &
SRV_PID=$!
sleep 1
for i in $(seq 1 50); do
    curl -sf "$API/healthz" >/dev/null 2>&1 && break
    sleep 0.2
done
# 对账断言：只看重启后新增的日志，且必须真的重置了 downloading 任务
tail -n +$((LOG_LINES_BEFORE + 1)) "$LOG" | grep -qE "reconcile: [1-9][0-9]* rows, [1-9][0-9]* reset downloading->pending" \
    && ok "reconcile reset mid-download task to pending" || bad "reconcile did not reset any downloading task"
ST=""
for i in $(seq 1 300); do
    ST=$(curl -sf "$API/api/task/$TASK_ID4" | python3 -c 'import json,sys; print(json.load(sys.stdin)["status"])')
    [[ "$ST" == "success" || "$ST" == "failed" ]] && break
    sleep 1
done
OUT_SHA4=$(sha1sum "$STORAGE/t4.bin" 2>/dev/null | awk '{print $1}')
[[ "$ST" == "success" && "$OUT_SHA4" == "$SRC_SHA" ]] && ok "reconciled task finished, checksum ok" || bad "reconcile: status=$ST sha=$OUT_SHA4"

# ---------- 7. T5 失败原因：不存在的文件 → failed + fail_reason ----------
echo "-- T5: failure reason recorded"
TASK_ID5=$(curl -sf -X POST "$API/api/task/submit" -d "{\"url\":\"http://127.0.0.1:$FILE_PORT/no_such_file.bin\",\"save_path\":\"t5.bin\",\"user_id\":\"u5\"}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["task_id"])')
ST=""; REASON=""
for i in $(seq 1 30); do
    J=$(curl -sf "$API/api/task/$TASK_ID5")
    ST=$(echo "$J" | python3 -c 'import json,sys; print(json.load(sys.stdin)["status"])')
    REASON=$(echo "$J" | python3 -c 'import json,sys; print(json.load(sys.stdin)["fail_reason"])')
    [[ "$ST" == "failed" ]] && break
    sleep 1
done
[[ "$ST" == "failed" && -n "$REASON" ]] && ok "failed with reason: $REASON" || bad "status=$ST reason=$REASON"

# ---------- 8. T6 用户级限流（D7）：并发超限 → 429 ----------
echo "-- T6: per-user rate limit (max_active_per_user=2, slow server keeps tasks active)"
SUBMIT() { curl -s -X POST "$API/api/task/submit" -d "$1"; }
T6A_JSON=$(SUBMIT "{\"url\":\"http://127.0.0.1:$FILE_PORT/big.bin\",\"save_path\":\"t6a.bin\",\"user_id\":\"u6\"}")
T6B_JSON=$(SUBMIT "{\"url\":\"http://127.0.0.1:$FILE_PORT/big.bin\",\"save_path\":\"t6b.bin\",\"user_id\":\"u6\"}")
T6C_CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$API/api/task/submit" \
    -d "{\"url\":\"http://127.0.0.1:$FILE_PORT/big.bin\",\"save_path\":\"t6c.bin\",\"user_id\":\"u6\"}")
T6A_OK=$(echo "$T6A_JSON" | python3 -c 'import json,sys; print("ok" if "task_id" in json.load(sys.stdin) else "no")')
T6B_OK=$(echo "$T6B_JSON" | python3 -c 'import json,sys; print("ok" if "task_id" in json.load(sys.stdin) else "no")')
[[ "$T6A_OK" == "ok" && "$T6B_OK" == "ok" && "$T6C_CODE" == "429" ]] \
    && ok "429 on 3rd active task for user u6" || bad "t6a=$T6A_OK t6b=$T6B_OK t6c=$T6C_CODE"
# 收尾：取消这两个任务，别拖住脚本退出
T6A_ID=$(echo "$T6A_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("task_id",0))')
T6B_ID=$(echo "$T6B_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("task_id",0))')
curl -s -X POST "$API/api/task/$T6A_ID/cancel" >/dev/null
curl -s -X POST "$API/api/task/$T6B_ID/cancel" >/dev/null
sleep 2

# ---------- 9. 目录穿越防护 ----------
echo "-- T7: path traversal rejected"
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$API/api/task/submit" -d "{\"url\":\"http://127.0.0.1:$FILE_PORT/big.bin\",\"save_path\":\"/etc/passwd\",\"user_id\":\"u7\"}")
[[ "$CODE" == "400" ]] && ok "absolute path outside root rejected (400)" || bad "code=$CODE"

echo ""
echo "== smoke result: PASS=$PASS FAIL=$FAIL =="
echo "== server log tail =="
tail -30 "$LOG" 2>/dev/null
[[ "$FAIL" == "0" ]]
