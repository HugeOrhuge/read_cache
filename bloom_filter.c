#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bloom_filter.h"

/* 每个 read_id 的布隆过滤器状态。 */
struct bloom_filter {
	uint8_t		*bits;
	size_t		nbytes;
	unsigned int	hashes;
};

/* 全局布隆过滤器数组，按 read_id 索引。 */
static struct bloom_filter *filters;
static unsigned int filters_max_ids;

/* 对文件路径字符串使用 FNV-1a 哈希。 */
static uint32_t hash_fnv1a(const char *str)
{
	uint32_t hash = 2166136261u;
	const unsigned char *p = (const unsigned char *)str;

	while (*p) {
		/* 使用下一个字节更新哈希。 */
		hash ^= *p++;
		hash *= 16777619u;
	}

	return hash;
}

/* 对文件路径字符串使用 djb2 哈希。 */
static uint32_t hash_djb2(const char *str)
{
	uint32_t hash = 5381u;
	const unsigned char *p = (const unsigned char *)str;

	while (*p)
		/* 使用下一个字节更新哈希。 */
		hash = ((hash << 5) + hash) + *p++;

	return hash;
}

/* 在布隆过滤器位图中设置位。 */
static void bloom_set_bit(struct bloom_filter *bf, uint32_t bit)
{
	uint32_t byte = bit >> 3;
	uint32_t mask = 1u << (bit & 7);

	bf->bits[byte] |= mask;
}

/* 测试布隆过滤器位图中的位。 */
static int bloom_test_bit(struct bloom_filter *bf, uint32_t bit)
{
	uint32_t byte = bit >> 3;
	uint32_t mask = 1u << (bit & 7);

	return !!(bf->bits[byte] & mask);
}

/* 为所有 read_id 分配布隆过滤器。 */
int bloom_filter_init(unsigned int max_read_ids, size_t bytes, unsigned int hashes)
{
	unsigned int i;
	int ret = 0;

	if (!max_read_ids || !bytes || !hashes)
		return -EINVAL;

	/* 为所有 read_id 分配过滤器数组。 */
	filters = calloc(max_read_ids, sizeof(*filters));
	if (!filters)
		return -ENOMEM;

	for (i = 0; i < max_read_ids; i++) {
		/* 为每个 read_id 分配位图。 */
		filters[i].bits = calloc(1, bytes);
		if (!filters[i].bits) {
			ret = -ENOMEM;
			goto out_free;
		}
		filters[i].nbytes = bytes;
		filters[i].hashes = hashes;
	}

	filters_max_ids = max_read_ids;

	return 0;

out_free:
	while (i--) {
		/* 失败时释放已分配的位图。 */
		free(filters[i].bits);
		filters[i].bits = NULL;
	}
	/* 失败时释放过滤器数组。 */
	free(filters);
	filters = NULL;

	return ret;
}

/* 清空指定 read_id 的布隆过滤器位图。 */
int bloom_filter_reset_read_id(uint32_t read_id)
{
	if (!filters || read_id >= filters_max_ids)
		return -EINVAL;

	/* 清空指定 read_id 的全部位。 */
	memset(filters[read_id].bits, 0, filters[read_id].nbytes);

	return 0;
}

/* 对指定 read_id 和路径执行布隆过滤器设置/测试。 */
static int bloom_filter_apply(uint32_t read_id, const char *file_path,
			     int set_bit)
{
	struct bloom_filter *bf;
	uint32_t h1, h2;
	uint32_t m;
	unsigned int i;

	if (!filters || !file_path)
		return -EINVAL;
	if (read_id >= filters_max_ids)
		return -EINVAL;

	bf = &filters[read_id];
	m = bf->nbytes * 8u;

	/* 生成双哈希所需的两个基础哈希。 */
	h1 = hash_fnv1a(file_path);
	h2 = hash_djb2(file_path) | 1u;

	for (i = 0; i < bf->hashes; i++) {
		uint32_t bit = (h1 + i * h2) % m;
		if (set_bit)
			/* 插入时设置位。 */
			bloom_set_bit(bf, bit);
		else if (!bloom_test_bit(bf, bit))
			/* 查询时某位未置位则一定不存在。 */
			return 0;
	}

	return 1;
}

/* 将文件路径插入指定 read_id 的布隆过滤器。 */
int bloom_filter_set(uint32_t read_id, const char *file_path)
{
	/* 将文件路径插入布隆过滤器。 */
	return bloom_filter_apply(read_id, file_path, 1);
}

/* 查询布隆过滤器并构建候选 read_id 列表。 */
int bloom_filter_query(const char *file_path, struct read_id_list **out_list)
{
	struct read_id_list *head = NULL;
	struct read_id_list *tail = NULL;
	unsigned int i;
	int count = 0;
	int ret;

	if (!filters || !out_list || !file_path)
		return -EINVAL;

	for (i = 0; i < filters_max_ids; i++) {
		/* 逐个检查每个 read_id 的过滤器。 */
		ret = bloom_filter_apply(i, file_path, 0);
		if (ret <= 0)
			continue;

		if (!head) {
			/* 首次命中创建头结点。 */
			head = calloc(1, sizeof(*head));
			if (!head)
				goto out_free;
			head->id = i;
			tail = head;
		} else {
			/* 后续命中追加节点。 */
			tail->next = calloc(1, sizeof(*tail->next));
			if (!tail->next)
				goto out_free;
			tail = tail->next;
			tail->id = i;
		}
		count++;
	}

	*out_list = head;
	return count;

out_free:
	/* 分配失败时释放部分链表。 */
	bloom_filter_free_list(head);
	return -ENOMEM;
}

/* 释放候选 read_id 链表。 */
void bloom_filter_free_list(struct read_id_list *list)
{
	struct read_id_list *next;

	while (list) {
		/* 释放前保存 next 指针。 */
		next = list->next;
		free(list);
		list = next;
	}
}
