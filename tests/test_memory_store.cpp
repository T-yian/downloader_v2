// MemoryTaskStore：TaskStore 接口语义测试（与 MysqlTaskStore 同构，内核可替换性验证）

#include "dlsvc/core/memory_store.h"

#include <cstdio>

#include "check.h"

using namespace dlsvc;

int main() {
    MemoryTaskStore store;

    // 无记录 → 空且 ok
    std::vector<SegmentRow> segs;
    CHECK(store.loadSegments(1, segs));
    CHECK(segs.empty());

    // 计划替换 → 读回一致
    std::vector<SegmentRow> plan = {
        {0, 0, 99, 0, false},
        {1, 100, 199, 0, false},
    };
    CHECK(store.replaceSegments(1, plan));
    CHECK(store.loadSegments(1, segs));
    CHECK(segs.size() == 2);
    CHECK(segs[0].segStart == 0 && segs[0].segEnd == 99);

    // 增量落账（upsert）→ 进度覆盖
    std::vector<SegmentRow> snapshot = {
        {0, 0, 99, 50, false},
        {1, 100, 199, 100, true},
    };
    CHECK(store.saveSegments(1, snapshot));
    CHECK(store.loadSegments(1, segs));
    CHECK(segs[0].downloaded == 50);
    CHECK(segs[1].finished == true);

    // 成功终态 → 分片账本清理
    CHECK(store.setTaskTerminal(1, TaskStatus::Success, "", 200, 0, {TaskStatus::Downloading}));
    CHECK(store.loadSegments(1, segs));
    CHECK(segs.empty());

    // 失败终态 → 账本保留（可续传）
    CHECK(store.replaceSegments(2, plan));
    CHECK(store.setTaskTerminal(2, TaskStatus::Failed, "network down", 20, 3,
                               {TaskStatus::Downloading}));
    CHECK(store.loadSegments(2, segs));
    CHECK(segs.size() == 2);

    printf("test_memory_store: all pass\n");
    return 0;
}
