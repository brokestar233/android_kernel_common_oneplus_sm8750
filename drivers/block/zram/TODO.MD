# zRAM 优化实施方案

## Phase 1: 基础设施与熔断机制 (Infrastructure & Safety)
**目标**: 建立物理层连续分配能力，并确保在系统高负载下能自动停止回写，防止死锁。

### P1 (Backend): 增强型块分配器
- [X] **1.1 实现连续块搜索算法**
    - 文件: `zram_wb.c`
    - 函数: `alloc_block_bdev_batch(zram, int count)`
    - 逻辑: 使用 `bitmap_find_next_zero_area` 在 backing device 的位图中寻找连续 N 个空闲位。
- [X] **1.2 实现分配降级策略 (Fallback)**
    - 逻辑: 请求 64 块 (256KB) -> 失败则尝试 32 块 -> 失败则尝试 16 块 -> 失败则尝试 8 块 -> 失败则尝试 4 块 ->最终降级为 1 块。
    - 目的: 防止由于磁盘碎片化导致无法回写。

### P3: 读写流量监控与熔断
- [ ] **1.3 扩展统计结构**
    - 文件: `zram_drv.h` -> `struct zram_stats`
    - 新增: `atomic64_t recent_reads` (Swap-in 计数) 和 `atomic64_t recent_writes` (Swap-out 计数)。
- [ ] **1.4 监控逻辑实现**
    - 文件: `zram_drv.c` -> `monitor_func`
    - 逻辑: 每秒计算 `delta_reads`。
    - 阈值: 若 `delta_reads > THRESHOLD` 或 `read/write ratio > 2.0`，置位 `zram->stop_writeback = true`。
- [ ] **1.5 阻断 Shrinker**
    - 文件: `zram_drv.c` -> `zram_shrinker_scan`
    - 逻辑: 入口处检查 `stop_writeback`，若为真，直接返回 `SHRINK_STOP`。

---

## Phase 2: 聚合写流水线 (The Batch Write Pipeline)
**目标**: 打通前端扫描与后端写入，实现从“逻辑相邻”到“物理相邻”的映射，消除写放大。

### P0: 窗口扫描 (Window-based Scanning)
- [ ] **2.1 改造 LRU 遍历逻辑**
    - 文件: `zram_drv.c` -> `zram_shrink_cb`
    - 逻辑: 
        1. LRU 链表仅作为“种子 (Seed)”选择器，选中 `page_index`。
        2. **脱离 LRU**，直接访问 `zram->table` 数组，扫描 `[page_index - 8, page_index + 8]` 范围。
- [ ] **2.2 实现聚类筛选**
    - 逻辑: 在窗口内统计满足 `is_idle && !under_writeback` 的页面数量。
    - 决策: 仅当有效页面数 >= 4 (最小聚合单位) 时，触发批量处理。

### P1 (Frontend): 构建大 BIO
- [X] **2.3 批量加锁与提取**
    - 逻辑: 批量获取窗口内页面的 `zram_slot_lock`，解压数据到临时 Buffer (或者直接利用页面引用)。
- [X] **2.4 构造 Multi-page BIO**
    - 逻辑: 调用 `bio_alloc` 创建一个包含多个 vector 的 BIO。
    - 映射: 将 Phase 1 申请到的连续 `sector` 填入 BIO。
- [ ] **2.5 Metadata 压缩存储**
    - 逻辑: 当页面写入 Flash 后，`zram_table_entry` 不再需要存储内存句柄。
    - 结构: 复用 `entry->handle` 字段 (配合 `ZRAM_WB` 标志)，存储 `cluster_offset` (4 bits) 和 `cluster_size` (4 bits)，为 Phase 3 做准备。

---

## Phase 3: 性能质变与预读 (Predictive Acceleration)
**目标**: 利用 P2 机制，将 Phase 2 的写入优化转化为用户的读取体验提升。

### P2: 智能全量预读 (Predictive Swap-in)
- [ ] **3.1 识别 Cluster 属性**
    - 文件: `zram_drv.c` -> `zram_read_page`
    - 逻辑: 读取缺页 entry 时，检查是否包含 `cluster_size > 1` 的元数据。
- [ ] **3.2 准入检查 (Safety)**
    - 逻辑: 检查 `zram_get_stat(ZRAM_COMP_DATA_SIZE)`。若 zRAM 内存占用 > 90%，**禁用预读**，仅读取单页 (防止预读导致 OOM)。
- [ ] **3.3 发起批量 IO**
    - 逻辑: 即使上层只请求了 1 个页，底层构造 BIO 读取整个 64KB (或对应 Cluster 大小) 的物理块。
- [ ] **3.4 批量回填 (Promotion)**
    - 逻辑: 
        1. BIO 回调后，解压请求的页面给用户。
        2. 将 Cluster 中其余页面**重新写入 zRAM 内存池** (Compress & Allocate)。
        3. 清除这些页面的 `ZRAM_WB` 标志 (视为已从磁盘取回)。

---

## Phase 4: 精细化运营 (QoS & Refinement)
**目标**: 在高性能基础上，增加对前台应用的保护。

### P4: Memcg 感知
- [ ] **4.1 记录 Memcg ID**
    - 文件: `zram_drv.c` -> `zram_write_page`
    - 逻辑: 获取 `current->mm->owner` 的 ID，存入 `zram_table_entry` (需利用 padding 或压缩现有字段)。
- [ ] **4.2 实现前台保护**
    - 文件: `zram_drv.c` -> `zram_shrink_cb`
    - 逻辑: 在 P0 扫描窗口时，跳过标记为 Foreground Memcg 的页面。

### P0+: 动态窗口调整
- [ ] **4.3 动态参数**
    - 逻辑: 根据 `zram_stats.recent_writes` 的压力，动态调整 P0 的扫描窗口大小 (e.g., 压力大时缩小窗口以减少 CPU 消耗，压力小时增大窗口以追求极致压缩率)。