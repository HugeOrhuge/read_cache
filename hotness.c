#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "read_cache.h"
#include "hotness.h"

/* 每个 read_id 的热度统计条目。 */
struct hotness_entry {
	uint32_t	id;
	uint64_t	hotness;
	int		used;
};

/* 内存热度表与写入指针。 */
static struct hotness_entry *hotness_tbl;
static unsigned int hotness_max_ids;
static uint32_t hotness_tail_id;
static unsigned int hotness_used;

/* 初始化热度表状态。 */
int hotness_init(unsigned int max_read_ids)
{
	if (!max_read_ids)
		return -EINVAL;

	/* 分配并清零热度表。 */
	hotness_tbl = calloc(max_read_ids, sizeof(*hotness_tbl));
	if (!hotness_tbl)
		return -ENOMEM;

	/* 初始化全局上限与分配指针。 */
	hotness_max_ids = max_read_ids;
	/* 令首次分配得到 0 号 read_id。 */
	hotness_tail_id = max_read_ids - 1;
	hotness_used = 0;

	return 0;
}

/* 判断 read_id 是否处于使用状态。 */
int hotness_is_used(uint32_t read_id)
{
	if (!hotness_tbl || read_id >= hotness_max_ids)
		return -EINVAL;

	/* 返回该 read_id 的使用标记。 */
	return hotness_tbl[read_id].used;
}

/* 重置指定 read_id 的热度。 */
int hotness_set_read_id_stats(uint32_t read_id)
{
	if (!hotness_tbl || read_id >= hotness_max_ids)
		return -EINVAL;

	/* 重置热度并标记为已使用。 */
	hotness_tbl[read_id].id = read_id;
	hotness_tbl[read_id].hotness = 0;
	if (!hotness_tbl[read_id].used)
		hotness_used++;
	hotness_tbl[read_id].used = 1;

	return 0;
}

/* 增加指定 read_id 的热度计数。 */
int hotness_inc_read_id(uint32_t read_id)
{
	if (!hotness_tbl || read_id >= hotness_max_ids)
		return -EINVAL;
	if (!hotness_tbl[read_id].used)
		return -EINVAL;

	/* 热度计数加一。 */
	hotness_tbl[read_id].hotness++;

	return 0;
}

/* 直接根据 read_id 更新热度。 */
int hotness_update_from_path(uint32_t read_id)
{
	/* 增加指定 read_id 的热度。 */
	return hotness_inc_read_id(read_id);
}

/* 将 read_id 追加为最新写入记录。 */
int hotness_mark_written(uint32_t read_id)
{
	if (!hotness_tbl)
		return -EINVAL;
	if (read_id >= hotness_max_ids)
		return -EINVAL;
	if (!hotness_tbl[read_id].used)
		return -EINVAL;
	if (hotness_used >= hotness_max_ids)
		return -ENOSPC;

	if (read_id != (hotness_tail_id + 1) % hotness_max_ids)
		return -EINVAL;

	hotness_tail_id = read_id;

	return 0;
}

/* 释放指定 read_id 的使用状态。 */
int hotness_release_read_id(uint32_t read_id)
{
	if (!hotness_tbl || read_id >= hotness_max_ids)
		return -EINVAL;

	if (hotness_tbl[read_id].used)
		hotness_used--;
	hotness_tbl[read_id].id = read_id;
	hotness_tbl[read_id].hotness = 0;
	hotness_tbl[read_id].used = 0;

	return 0;
}

/* 返回热度最低的 read_id。 */
int hotness_get_min_read_id(uint32_t *read_id)
{
	uint64_t min = UINT64_MAX;
	uint32_t min_id = 0;
	unsigned int i;
	int found = 0;

	if (!hotness_tbl || !read_id)
		return -EINVAL;

	/* 遍历已使用条目以寻找最小热度。 */
	for (i = 0; i < hotness_max_ids; i++) {
		if (!hotness_tbl[i].used)
			continue;
		if (hotness_tbl[i].hotness < min) {
			min = hotness_tbl[i].hotness;
			min_id = hotness_tbl[i].id;
			found = 1;
		}
	}

	if (!found)
		return -ENOENT;

	*read_id = min_id;
	return 0;
}

/* 生成 read_XXX 形式的目录路径。 */
int hotness_get_read_id_dir(uint32_t read_id, char *out_dir, size_t out_len)
{
	int ret;

	if (!out_dir || !out_len)
		return -EINVAL;

	/* 拼接 read_XXX 目录路径字符串。 */
	ret = snprintf(out_dir, out_len, READ_ID_DIR_FMT, read_id);
	if (ret < 0 || (size_t)ret >= out_len)
		return -ENAMETOOLONG;

	return 0;
}

/* 以循环方式分配 read_id 并重置热度。 */
int hotness_alloc_read_id(uint32_t *read_id, char *out_dir, size_t out_len)
{
	uint32_t id;
	unsigned int i;
	int ret;

	if (!hotness_tbl || !read_id)
		return -EINVAL;

	if (hotness_used >= hotness_max_ids)
		return -ENOSPC;

	/* 从 tail 的下一个位置开始寻找最近的空闲 read_id。 */
	for (i = 0; i < hotness_max_ids; i++) {
		id = (hotness_tail_id + 1 + i) % hotness_max_ids;
		if (!hotness_tbl[id].used)
			break;
	}
	if (i == hotness_max_ids)
		return -ENOSPC;

	*read_id = id;

	/* 重置已分配 read_id 的热度。 */
	ret = hotness_set_read_id_stats(id);
	if (ret)
		return ret;

	/* 如需要则为调用方生成目录字符串。 */
	if (out_dir)
		return hotness_get_read_id_dir(id, out_dir, out_len);

	return 0;
}
