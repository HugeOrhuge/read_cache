#ifndef BLOOM_FILTER_H
#define BLOOM_FILTER_H

#include <stddef.h>
#include <stdint.h>

#include "list.h"

/* read_id 候选链表节点。 */
struct read_id_list {
	uint32_t		id;
	struct list_head	list;
};

/* 初始化所有 read_id 的布隆过滤器。 */
int bloom_filter_init(unsigned int max_read_ids, size_t bytes, unsigned int hashes);
/* 重置指定 read_id 的布隆过滤器位图。 */
int bloom_filter_reset_read_id(uint32_t read_id);
/* 为某个 read_id 的文件设置布隆过滤器位。 */
int bloom_filter_set(uint32_t read_id, const char *file_path);
/* 查询文件路径可能命中的 read_id 列表。 */
int bloom_filter_query(const char *file_path, struct list_head *out_list);
/* 释放 read_id 候选链表。 */
void bloom_filter_free_list(struct list_head *list);

#endif
