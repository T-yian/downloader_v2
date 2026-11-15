-- ============================================================
-- 下载器 v2 元数据账本（D3 双表设计）
--   task              任务级：状态机 / 进度 / 失败原因 / 重试计数
--   segment_progress  分片级：断点恢复精度（分片粒度决定续传起点精度）
--
-- 执行：mysql -uroot -p < sql/schema.sql
-- ============================================================

CREATE DATABASE IF NOT EXISTS downloader DEFAULT CHARACTER SET utf8mb4;
USE downloader;

CREATE TABLE IF NOT EXISTS task (
    id               BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    user_id          VARCHAR(64)   NOT NULL DEFAULT 'default',
    url              TEXT          NOT NULL COMMENT '应用层限制 ≤2048 字符',
    save_path        VARCHAR(1024) NOT NULL,
    -- 断点匹配键（D3）：匹配键收敛为 url + save_path 单键。
    -- 为什么是哈希列而不是 (url, save_path) 联合唯一索引：
    --   url 最长 2048，超出 InnoDB 索引长度上限；降级为前缀索引会有误碰撞。
    --   定长 CHAR(40) 唯一索引，冲突语义完全可控。
    resume_key       CHAR(40)      NOT NULL COMMENT 'sha1(url + 0x1f + save_path)',
    total_size       BIGINT        NOT NULL DEFAULT -1 COMMENT '-1 = 未探测到',
    downloaded_bytes BIGINT        NOT NULL DEFAULT 0,
    status           VARCHAR(16)   NOT NULL DEFAULT 'pending'
                     COMMENT 'pending/downloading/success/failed/canceled',
    fail_reason      VARCHAR(512)  NOT NULL DEFAULT '',
    retry_count      INT           NOT NULL DEFAULT 0,
    created_at       DATETIME(3)   NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       DATETIME(3)   NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                   ON UPDATE CURRENT_TIMESTAMP(3),

    UNIQUE KEY uk_resume_key (resume_key),
    -- D7 用户级限流：COUNT(*) WHERE user_id=? AND status IN ('pending','downloading')
    KEY idx_user_status (user_id, status),
    -- D5 重启对账扫描
    KEY idx_status_created (status, created_at)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COMMENT = '下载任务表';

CREATE TABLE IF NOT EXISTS segment_progress (
    task_id    BIGINT UNSIGNED NOT NULL,
    seg_index  INT    NOT NULL,
    seg_start  BIGINT NOT NULL COMMENT '闭区间起始',
    seg_end    BIGINT NOT NULL COMMENT '闭区间结束；降级单流未知总长时为 -1',
    downloaded BIGINT NOT NULL DEFAULT 0 COMMENT '相对 seg_start 的已下载偏移',
    finished   TINYINT NOT NULL DEFAULT 0,
    PRIMARY KEY (task_id, seg_index),
    CONSTRAINT fk_segment_task FOREIGN KEY (task_id) REFERENCES task (id) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COMMENT = '分片断点账本';

-- 可选：服务专用账号（示例用 dlsvc/dlsvc123，生产环境改掉）
-- CREATE USER IF NOT EXISTS 'dlsvc'@'localhost' IDENTIFIED BY 'dlsvc123';
-- GRANT ALL PRIVILEGES ON downloader.* TO 'dlsvc'@'localhost';
-- FLUSH PRIVILEGES;
