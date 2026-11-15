// 阻塞队列行为测试：FIFO / 容量阻塞 / 限时拒绝 / close 语义 / 多生产多消费守恒

#include "dlsvc/blocking_queue.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "check.h"

using namespace dlsvc;
using namespace std::chrono;

int main() {
    // 1. FIFO + close 语义：close 后 push 失败、存量可取空、取空后 pop 返回 false
    {
        BlockingQueue<int> q(4);
        CHECK(q.push(1));
        CHECK(q.push(2));
        int v = 0;
        CHECK(q.pop(v) && v == 1);
        q.close();
        CHECK(q.closed());
        CHECK(!q.tryPush(3, std::chrono::milliseconds(10)));
        CHECK(q.pop(v) && v == 2); // close 不清空存量
        CHECK(!q.pop(v));          // 取空后 false —— worker 的退出条件
    }

    // 2. 容量阻塞：满时 push 挂起，直到消费者腾出位置
    {
        BlockingQueue<int> q(2);
        CHECK(q.push(1));
        CHECK(q.push(2));
        auto consumer = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            int x = 0;
            CHECK(q.pop(x) && x == 1);
        });
        auto t0 = steady_clock::now();
        CHECK(q.push(3)); // 必须等 consumer pop 之后才返回
        auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        consumer.join();
        CHECK(elapsed >= 80); // 阻塞行为生效
        CHECK(!q.tryPush(4, std::chrono::milliseconds(50))); // 限时拒绝：仍然满
    }

    // 3. 多生产多消费：总数守恒 + close 后全线程干净退出
    {
        BlockingQueue<int> q(8);
        std::atomic<long long> sum{0};
        constexpr int kProducers = 4, kConsumers = 4, kPerProducer = 2000;

        std::vector<std::thread> threads;
        for (int p = 0; p < kProducers; ++p) {
            threads.emplace_back([&q] {
                for (int i = 1; i <= kPerProducer; ++i) CHECK(q.push(i));
            });
        }
        for (int c = 0; c < kConsumers; ++c) {
            threads.emplace_back([&q, &sum] {
                int v = 0;
                while (q.pop(v)) sum.fetch_add(v);
            });
        }
        for (int p = 0; p < kProducers; ++p) threads[p].join();
        q.close(); // 生产全完 → 关队列 → 消费者取完存量自然退出
        for (size_t i = kProducers; i < threads.size(); ++i) threads[i].join();

        long long expect = (long long)kProducers * ((long long)kPerProducer * (kPerProducer + 1) / 2);
        CHECK(sum.load() == expect);
    }

    printf("test_blocking_queue: all pass\n");
    return 0;
}
