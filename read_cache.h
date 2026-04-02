#ifndef READ_CACHE_H
#define READ_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "hotness.h"
#include "bloom_filter.h"

/* 内存元数据和布隆过滤器的默认参数。 */
#define READ_ID_MAX		256
#define BLOOM_FILTER_BYTES	(3 * 1024 * 1024)
#define BLOOM_FILTER_HASHES	10
#define READ_ID_DIR_FMT		"read_%03u"

/* 待写入 read_id 目录的内存文件描述。 */
struct packed_file {
	const char		*path;
	const void		*data;
	size_t			size;
	struct packed_file	*next;
};

/* 一个 packed zone 内的文件链表。 */
struct packed_zone {
	struct packed_file	*files;
	size_t			total_bytes;
};

/* 初始化热度表和布隆过滤器元数据。 */
int read_cache_init(void);
/* 检查设备空间是否足够写入该 packed zone。 */
int read_cache_check_space(const struct packed_zone *pz);
/* 删除 read_id 目录下所有文件并移除目录。 */
int read_cache_unlink_read_id_dir(const char *read_id_dir);
/* 将 packed zone 写入新分配的 read_id 目录。 */
int packed_zone_flush(struct packed_zone *pz);
/* 查询缓存文件，命中则返回打开的 fd。 */
int read_cache_query(const char *file_path);

#endif
