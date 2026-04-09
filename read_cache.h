#ifndef READ_CACHE_H
#define READ_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "hotness.h"
#include "bloom_filter.h"
#include "list.h"

/* 内存元数据和布隆过滤器的默认参数。 */
#define READ_ID_MAX		256
#define BLOOM_FILTER_BYTES	(3 * 1024 * 1024)
#define BLOOM_FILTER_HASHES	10
#define READ_ID_DIR_FMT		"read_%03u"
#define READ_CACHE_BLOCK_SIZE	4096
#define READ_CACHE_DEFAULT_PACKED_ZONE_BYTES	(1ULL << 30)

/* 待写入 read_id 目录的内存文件描述。 */
struct packed_file {
	const char		*path;
	const void		*data;
	size_t			size;
	struct list_head	list;
};

/* 一个 packed zone 内的文件链表。 */
struct packed_zone {
	struct list_head	files;
	size_t			total_bytes;
};

/* 初始化热度表和布隆过滤器元数据。 */
int read_cache_init(uint64_t read_id_size_bytes);
/* 设置用于 ioctl 的 f2fs 文件描述符。 */
int read_cache_set_fs_fd(int fd);
/* 检查设备空间是否足够写入该 packed zone。 */
int read_cache_check_space(const struct packed_zone *pz);
/* 设置/获取 packed_zone 刷写阈值（字节）。 */
int read_cache_set_packed_zone_threshold(size_t bytes);
size_t read_cache_get_packed_zone_threshold(void);
/* 启动/停止后台打包线程。 */
int read_cache_start_worker(void);
void read_cache_stop_worker(void);
/* 入队一个需要打包的文件（由后台线程读取）。 */
int read_cache_enqueue_file(const char *path, int fd);
/* 删除 read_id 目录下所有文件并移除目录。 */
int read_cache_unlink_read_id_dir(const char *read_id_dir);
/* 将文件读入内存并追加到 packed_zone。 */
int packed_zone_add_file_from_fd(struct packed_zone *pz, const char *path, int fd);
/* 判断 packed_zone 是否满足刷写阈值。 */
int packed_zone_should_flush(const struct packed_zone *pz);
/* 释放 packed_zone 中的所有文件数据。 */
void packed_zone_free(struct packed_zone *pz);
/* 将 packed zone 写入新分配的 read_id 目录。 */
int packed_zone_flush(struct packed_zone *pz);
/* 查询缓存文件，命中则返回打开的 fd。 */
int read_cache_query(const char *file_path);

#endif
