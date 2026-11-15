#pragma once

// 线程安全阻塞队列（mutex + 两把 condition_variable）——调度层的核心原语。
//
// 为什么不用无锁队列：
//   1) 这是任务级排队，粒度是"一个下载任务"，竞争频率是每任务一次，
//      无锁队列优化的恰恰不是这里的热点；
//   2) 需要的两个语义——满时阻塞/限时拒绝、close 后可唤醒全部消费线程退出——
//      用 condvar 表达最自然；无锁方案补齐这两个语义需要额外的内存序论证，
//      复杂度撑不起收益。

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace dlsvc {

template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t capacity) : capacity_(capacity) {}

    // 阻塞式入队；队列已 close 返回 false
    // 注意：这里必须用无超时的 wait。若传 milliseconds::max() 给 wait_for，
    // libstdc++ 内部计算 now()+timeout 会溢出 int64，wait_for 立即返回，
    // 队列满时 push 直接失败——容量阻塞语义整个失效
    bool push(T value) {
        std::unique_lock<std::mutex> lk(mu_);
        notFull_.wait(lk, [this] { return closed_ || q_.size() < capacity_; });
        if (closed_) return false;
        q_.push_back(std::move(value));
        notEmpty_.notify_one();
        return true;
    }

    // 限时入队：超时或已 close 返回 false（提交接口用它实现"队列满 -> 503"）
    template <typename Rep, typename Period>
    bool tryPush(T value, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!notFull_.wait_for(lk, timeout, [this] { return closed_ || q_.size() < capacity_; }))
            return false; // 超时：满
        if (closed_) return false;
        q_.push_back(std::move(value));
        notEmpty_.notify_one();
        return true;
    }

    // 阻塞式出队；close 且取空后返回 false（worker 的退出条件）
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(mu_);
        notEmpty_.wait(lk, [this] { return closed_ || !q_.empty(); });
        if (q_.empty()) return false; // closed && drained
        out = std::move(q_.front());
        q_.pop_front();
        notFull_.notify_one();
        return true;
    }

    // 关闭队列：唤醒所有等待者；push 立即失败，pop 把存量取完后返回 false
    void close() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return q_.size();
    }

    size_t capacity() const { return capacity_; }

    bool closed() const {
        std::lock_guard<std::mutex> lk(mu_);
        return closed_;
    }

private:
    mutable std::mutex mu_;
    std::condition_variable notEmpty_; // pop 等它
    std::condition_variable notFull_;  // push 等它
    std::deque<T> q_;
    size_t capacity_;
    bool closed_ = false;
};

} // namespace dlsvc
