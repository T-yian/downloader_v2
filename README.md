# downloader-v2

downloader-v2 是一个 c++17 Linux 后端项目：异步文件拉取调度服务——接收外部下发的下载任务（url + save_path），服务端完成远端资源分片下载与断点续传，文件持久化在服务端磁盘，调用方只关心任务成败。适用场景：模型权重 / 数据集预缓存、推理集群的前置文件缓存节点。

项目重点不是「能跑」：接入 / 调度 / 执行三层各有可解释的设计取舍，配套单测、全链路冒烟、边界场景、压测四层验证，结论全部可复现。

## 项目总览

- **技术栈**：C++17、cpp-httplib、libcurl、MySQL（libmysqlclient / libmariadb 任一）、POSIX（pwrite / ftruncate / fsync / rename）
- **系统能力**：HTTP 接入 + 阻塞队列排队 + worker 线程池执行；分片下载、断点续传、容错降级；两级并发限流；进度节流落库；重启对账
- **工程亮点**：
  - JSON / SHA-1 / 日志零第三方依赖（`mini_json` / `sha1` / `log` 自带实现）
  - 预分配文件 + pwrite 分片写盘：各分片写互不重叠的区间，无文件锁并发
  - fsync + rename 原子发布 + 目录 fsync，掉电级别完整性
  - 任务状态机全部条件更新（`UPDATE ... WHERE status IN (...)`），终态不可回退
  - 断点账本 task + segment_progress 双表，分片粒度续传
  - 四层验证：ctest 单测 / smoke 全链路 14 项 / edge 边界 13 项 / stress 压测，数字可复现

## 总体架构

```
用户 / 推理服务
    │ HTTP (cpp-httplib)
    ▼
┌──────────────── 接入层 http_api ────────────────┐
│ POST /api/task/submit      提交任务（校验+入库+入队）
│ GET  /api/task/{id}        查询状态/进度/分片断点
│ POST /api/task/{id}/cancel 取消（协作式）
│ GET  /api/tasks            任务列表
└────────────────┬────────────────────────────────┘
                 ▼
      BlockingQueue（mutex + condvar）
                 │ 取 task_id
                 ▼
   worker 线程池（规模 = 全局并发上限）
                 │ 调用
                 ▼
      DownloadRunner 下载内核
      libcurl 分片 / 断点续传 / 容错降级
                 │ 写回调（块级取消检查）
                 ▼
   .dltmp 预分配文件(pwrite) ◄──► MySQL task + segment_progress
                 ▼ 成功
      fsync + rename 原子发布
```

## 快速开始（5 分钟可跑通）

**1) 安装依赖**

```bash
sudo apt update
sudo apt install -y build-essential cmake libcurl4-openssl-dev libmysqlclient-dev
```

**2) 建库建表**

```bash
mysql -uroot -p < downloader-v2/sql/schema.sql
```

**3) 编译与运行**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
mkdir -p /data/dlsvc
cp downloader-v2/config/download_server.conf.example downloader-v2/config/download_server.conf
./build/download_server -c downloader-v2/config/download_server.conf
```

日志出现 `ready: http://0.0.0.0:9090` 即启动成功。命令行内核工具（无服务无 DB）：

```bash
./build/dlcli -o /tmp/x.bin -s 4 http://127.0.0.1:8000/big.bin
```

## 接口与状态码（核心）

| 接口 | 作用 | 返回 |
|---|---|---|
| `POST /api/task/submit` | 提交任务 | 200 `{task_id, resumed}`；400 参数/路径非法；409 同键任务已成功或进行中；429 用户并发超限；503 队列满 |
| `GET /api/task/{id}` | 状态/进度/分片断点 | 200；404 不存在 |
| `POST /api/task/{id}/cancel` | 协作式取消 | 200；409 已终态；404 |
| `GET /api/tasks?status=&user_id=&limit=` | 任务列表 | 200 |
| `GET /healthz` | 活跃任务/队列深度/worker 数 | 200 |

```bash
curl -s -X POST http://127.0.0.1:9090/api/task/submit \
     -d '{"url":"http://example.com/model.bin","save_path":"models/model.bin","user_id":"cv-platform"}'
# {"task_id":1,"resumed":false}
# resumed=true 表示复活了 failed/canceled 的旧任务，从断点继续
```

进行中任务的进度读内存态（节流窗口内比 DB 新）；failed/canceled 会带回分片断点。save_path 相对路径自动落到 `storage_root` 下，逃逸出根目录直接 400（词法归一化防目录穿越）。

## 关键设计与优化

**1) 双层并发约束**
全局并发 = worker 线程池规模（磁盘 IO/带宽的第一道闸）；任务内部还有分片多连接，总连接 ≈ `worker_count × max_segments_per_task`，容量规划按乘积算。

**2) 进度节流落库**
下载回调只改内存原子计数；独立 flusher 线程按时间窗口（默认 2s）或字节增量（默认 8MB）批量落库。落库失败不推进 flushedBytes，恢复后自动补写——进度只延迟持久化，不丢失。

**3) 断点账本与条件更新状态机**
task 表（任务级）+ segment_progress 表（分片级）；断点匹配键收敛为 url+save_path 的 sha1 定长哈希列（长 url 超出 InnoDB 索引长度上限，前缀索引有误碰撞风险）。状态迁移全部带 `UPDATE ... WHERE status IN (...)` 条件：终态不可回退，cancel 与 worker 收尾谁先迁移谁生效。

**4) 阻塞队列的语义选择**
mutex + 两把 condvar，提供满时阻塞 / 限时拒绝 / close 后唤醒全部消费者取完退出三个语义。不用无锁队列：排队粒度是"任务"，竞争频率低，condvar 表达这三个语义最自然。

**5) 分片写盘：预分配 + pwrite**
ftruncate 预分配到目标大小，各分片 pwrite 各自区间——无文件锁、无写冲突；成功收尾 fsync + rename 原子发布，目录 fsync 保证 rename 持久化。

**6) 协作式取消**
cancel 只置内存标志 + 条件更新 DB；内核在每个写盘块开头检查标志，命中即退出。被取消任务保留 `.dltmp` 与分片账本，同 url+save_path 再提交即从断点续传。

**7) 重启对账**
启动扫描 `status IN (pending, downloading)`：downloading 是残留态（worker 已不存在）→ 重置 pending；pending 重新入队（队列在内存，进程崩溃即丢，靠 DB 恢复）。

**8) 两级限流**
全局 = 池大小（不为限流另造机制）；用户级 = 提交时刻按 `(user_id, status)` 索引统计，超限 429。队列满 = 提交 503 并回滚任务行。

**9) 容错降级与重试**
探测 HEAD 优先，被拒（405/501）或缺头时 `Range: bytes=0-0` GET 兜底解析 Content-Range；不支持 Range / 总长未知 → 单流降级，按文件大小续传。网络错误 / 5xx / 低速卡死（30s 吞吐 < 1KB/s）自动重试，退避 n×backoff；4xx（除 408/429）与"分片请求被回 200"判永久失败不空转。每分片收到字节必须精确等于区间长度，全部匹配才发布。

## 性能与稳定性（可复现结论）

已使用自动化脚本在同一环境（WSL2 Ubuntu 24.04 / MySQL 8 / loopback + NVMe）做全量回归：

- **全链路冒烟 14 项**：下载校验和一致；取消续传一致；kill -9 重启对账续传一致；用户级 429；目录穿越 400
- **边界 13 项**：队列满 503；DB 断连期间下载照常完成（校验和一致）+ 重启对账收敛回 success；磁盘满 ENOSPC 快速失败（fail_reason 逐分片记录 pwrite 偏移）
- **压测**：16 并发任务 × 200MB = 3.2GB，聚合吞吐 ~305 MB/s（瓶颈在 Python 测试文件服务器与磁盘写入，不在服务逻辑），16/16 成功，落盘文件 sha1 抽查一致；峰值 active 恒等于 worker_count=8（全局上限被池规模精确钳制），队列 34→0 正常排空

## 测试与验证

```bash
cd build && ctest --output-on-failure      # 5 个单测：队列/JSON/工具/SHA-1/账本语义
bash scripts/smoke_test.sh                 # 14 项全链路（自带 Range 测试文件服务器）
sudo bash scripts/edge_test.sh             # 13 项边界（root：挂 tmpfs + 控制 mysqld）
bash scripts/stress_test.sh 16 200         # 压测：吞吐/排队深度/完整性抽查
```

## 已知边界与后续优化方向

- DB 断连期间提交/查询接口返回 500（DB 是事实源，不做降级假响应）；运行期 worker 拾取任务遇 DB 不可读会退避重入队，存量任务靠重启对账收敛
- 远端文件内容变化（同 URL 不同内容）不做 ETag / Last-Modified 校验
- "分片请求被回 200"的谎报 Range 服务器判永久失败，未做自动降级重试（探测已挡掉绝大多数）
- 后续：任务级看门狗超时与僵尸 downloading 清理定时器；压测并发梯度报告（P50/P99 延迟曲线）；文件取回接口；简单 token 鉴权；双平台构建

## 常见问题

- **为什么阻塞队列不用无锁队列？** 排队粒度是"任务"（每任务一次竞争），无锁优化不到热点；满阻塞 / 限时拒绝 / close 唤醒三个语义用 condvar 最自然，无锁补齐需要额外的内存序论证，复杂度撑不起收益。
- **为什么进度不每块写库？** 256KB 一块、10MB/s 即每秒 40 次写库，DB 会被打成热点；节流窗口 = 崩溃时最多丢失的进度秒数，精度与 DB 压力的折衷。
- **服务重启后任务怎么办？** DB 是事实源：启动对账把 downloading 重置 pending 并全部重新入队；文件与分片账本都在，从断点继续。
- **取消为什么是协作式？** 强杀线程会把文件与账本留在未知状态；协作式退出保证断点完整，文件可续传。
- **resume_key 为什么是哈希列而不是 (url, save_path) 联合索引？** url 最长 2048 超出 InnoDB 索引长度上限；前缀索引有误碰撞风险；定长 CHAR(40) 唯一索引语义可控。
- **配置里的 mysql 账号 dlsvc/dlsvc123 是什么？** 本地开发占位账号，由 `sql/schema.sql` 末尾的注释段创建，仅授权 downloader 库；生产环境替换。

## 目录结构

- `downloader-v2/` — 项目主体
  - `include/dlsvc` — 头文件：core 内核 / db DAO / scheduler 调度 / api 接口 / 基础库
  - `src` — 对应实现 + `main_server.cpp`（服务入口）+ `main_cli.cpp`（命令行内核工具）
  - `sql` — 建库建表
  - `config` — 默认配置示例
  - `third_party/httplib` — cpp-httplib v0.15.3（header-only）
- `tests` — 单元测试（零第三方依赖）
- `scripts` — smoke / edge / stress 验收脚本 + Range 测试文件服务器
- `CMakeLists.txt` — 顶层构建入口，可执行文件统一输出到 `build/download_server`、`build/dlcli`
