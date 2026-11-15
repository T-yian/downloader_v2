#include "dlsvc/scheduler/progress_flusher.h"

#include <chrono>

#include "dlsvc/log.h"

namespace dlsvc {

ProgressFlusher::ProgressFlusher(TaskDao& dao, SegmentDao& segDao, SnapshotFn fn,
                                 long long intervalMs, long long flushBytes)
    : dao_(dao), segDao_(segDao), snapshot_(std::move(fn)), intervalMs_(intervalMs),
      flushBytes_(flushBytes) {}

ProgressFlusher::~ProgressFlusher() { stop(); }

void ProgressFlusher::start() {
    if (th_.joinable()) return;
    stop_.store(false);
    th_ = std::thread([this] { loop(); });
}

void ProgressFlusher::stop() {
    if (!th_.joinable()) return;
    stop_.store(true);
    th_.join();
}

void ProgressFlusher::flushOne(TaskContext& ctx, const std::vector<SegmentRow>& rows) {
    // 先落分片账本，再落任务级进度；两步都成功才推进 flushedBytes。
    // 任何一步失败 → flushedBytes 不动 → 下一轮把当前值整批重写（幂等，重复写无害）
    if (!rows.empty() && !segDao_.upsertProgress(ctx.id, rows)) {
        LOG_WARN("flusher: task %lld segment upsert failed, will retry", ctx.id);
        return;
    }
    if (!dao_.updateProgress(ctx.id, ctx.downloadedBytes.load(), -1)) {
        LOG_WARN("flusher: task %lld progress update failed, will retry", ctx.id);
        return;
    }
    ctx.flushedBytes = ctx.downloadedBytes.load();
    ctx.lastFlushTime = std::chrono::steady_clock::now();
}

void ProgressFlusher::loop() {
    auto lastSweep = std::chrono::steady_clock::now();
    while (!stop_.load()) {
        // 200ms 切片睡眠：stop() 最迟 200ms 后被观察到
        for (int i = 0; i < 10 && !stop_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (stop_.load()) break;

        auto now = std::chrono::steady_clock::now();
        bool sweep = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSweep).count() >=
                     intervalMs_;
        if (!sweep && flushBytes_ <= 0) continue;

        for (const auto& ctx : snapshot_()) {
            if (stop_.load()) break;
            long long delta = ctx->downloadedBytes.load() - ctx->flushedBytes;
            if (delta <= 0) continue;
            // 双条件节流（D2）：时间窗口到，或窗口内增量超过字节兜底阈值
            if (!sweep && delta < flushBytes_) continue;

            std::vector<SegmentRow> rows;
            {
                std::lock_guard<std::mutex> lk(ctx->mu_);
                for (const auto& s : ctx->segments)
                    rows.push_back({s->segIndex, s->segStart, s->segEnd, s->downloaded.load(),
                                    s->finished.load()});
            }
            flushOne(*ctx, rows);
        }
        if (sweep) lastSweep = now;
    }
}

} // namespace dlsvc
