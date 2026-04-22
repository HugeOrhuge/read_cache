# Read Cache 与 F2FS 修改摘要

## Read Cache 逻辑
- 读缓存核心由 read_id 目录组织，路径格式为 `read_%03u`，每次刷写生成新的 read_id 目录。
- 写入流程通过 `packed_zone_flush()` 将 `packed_zone` 中的文件写入 read_id 目录：
  - 写入前通过 `F2FS_IOC_GET_FREE_ZONES` 检查 free zone，不足则按最小热度驱逐。
  - 驱逐时删除对应 read_id 目录，重置布隆过滤器并释放热度记录。
  - 写入成功后更新布隆过滤器与热度统计。
- 读查询通过布隆过滤器生成候选 read_id 列表，再在候选目录中尝试打开目标文件并增加热度。
- 热度表为内存数组，记录每个 read_id 的访问次数，驱逐时选择最小热度。
- 布隆过滤器按 read_id 维护位图，使用 FNV-1a 与 djb2 作为双哈希。
- 新增后台线程与入队机制：
  - `read_cache_start_worker()` 启动后台线程，`read_cache_stop_worker()` 停止并清理。
  - `read_cache_enqueue_file(path, fd)` 将文件入队，后台线程从 `fd` 读取完整文件并加入 `packed_zone`。
  - `packed_zone_should_flush()` 判断是否达到阈值，达到后自动刷写并释放内存。
- `packed_zone` 阈值可配置，默认 1GB：
  - `read_cache_set_packed_zone_threshold()` / `read_cache_get_packed_zone_threshold()`。

## F2FS 修改逻辑
- 新增 `F2FS_IOC_GET_FREE_ZONES` ioctl，导出 free zone 统计信息，基于 zone capacity 计算。
  - UAPI 结构为 `f2fs_free_zone_info`，包含 zone capacity、reserved_zones、prefree_zones、free_zones 等字段。
- 读缓存空间判断统一通过 `F2FS_IOC_GET_FREE_ZONES` 获取 free zone。
- read_id 规模动态计算：
  - `read_cache_init()` 使用 `free_zones * zone_capacity_blocks * 4096 / read_id_size_bytes` 计算上限，并预留固定数量。
- 引入 `spin_write` 专用写流：
  - 增加独立的 curseg 与保留 zone 池，普通分配避开保留区。
  - `stream_id` 仅内存态，不做持久化；通过 ioctl 设置/获取。
  - 支持挂载参数 `spin_write_zones=%u` 配置保留区数量，默认 48。
- 日志改为 `f2fs_info`，便于测试观测。

## 主要接口位置
- Read Cache 核心逻辑：
  - [read_cache/read_cache.c](read_cache.c)
  - [read_cache/read_cache.h](read_cache.h)
- 布隆过滤器：
  - [read_cache/bloom_filter.c](bloom_filter.c)
  - [read_cache/bloom_filter.h](bloom_filter.h)
- 热度表：
  - [read_cache/hotness.c](hotness.c)
  - [read_cache/hotness.h](hotness.h)
- F2FS ioctl 与结构：
  - [Zone-FS/linux-5.17.4/include/uapi/linux/f2fs.h](../Zone-FS/linux-5.17.4/include/uapi/linux/f2fs.h)

## 使用说明（简要）
- 初始化：调用 `read_cache_set_fs_fd()` 与 `read_cache_init()`。
- 启动后台线程：调用 `read_cache_start_worker()`。
- FUSE 首次获得 fd 时：调用 `read_cache_enqueue_file(path, fd)`。
- 关闭时：调用 `read_cache_stop_worker()`。
