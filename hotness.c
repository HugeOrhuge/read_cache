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

/* 内存热度表及分配游标。 */
static struct hotness_entry *hotness_tbl;
static unsigned int hotness_max_ids;
static uint32_t hotness_next_id;

/* 初始化热度表状态。 */
int hotness_init(unsigned int max_read_ids)
{
	if (!max_read_ids)
		return -EINVAL;

	/* 分配并清零热度表。 */
	hotness_tbl = calloc(max_read_ids, sizeof(*hotness_tbl));
	if (!hotness_tbl)
		return -ENOMEM;

	/* 初始化全局上限与分配游标。 */
	hotness_max_ids = max_read_ids;
	hotness_next_id = 0;

	return 0;
}

/* 判断 read_id 是否处于使用状态。 */
int hotness_is_used(uint32_t read_id)
{
	if (!hotness_tbl || read_id >= hotness_max_ids)
		return 0;

	/* 返回该 read_id 的使用标记。 */
	return hotness_tbl[read_id].used;
}

/* 从 read_id/<id>/ 路径解析 read_id。 */
static int hotness_parse_read_id(const char *path, uint32_t *read_id)
{
	const char *prefix = READ_ID_DIR_NAME "/";
	const char *p;
	char *endp;
	unsigned long id;

	if (!path || !read_id)
		return -EINVAL;

	/* 确保路径以 read_id/ 前缀开头。 */
	if (strncmp(path, prefix, strlen(prefix)))
		return -EINVAL;

	/* 解析数字 read_id 并校验分隔符。 */
	p = path + strlen(prefix);
	id = strtoul(p, &endp, 10);
	if (endp == p || *endp != '/')
		return -EINVAL;
	if (id > UINT32_MAX)
		return -EINVAL;

	*read_id = (uint32_t)id;
	return 0;
}

/* 重置指定 read_id 的热度。 */
int hotness_reset_read_id(uint32_t read_id)
{
	if (!hotness_tbl || read_id >= hotness_max_ids)
		return -EINVAL;

	/* 重置热度并标记为已使用。 */
	hotness_tbl[read_id].id = read_id;
	hotness_tbl[read_id].hotness = 0;
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

/* 从路径提取 read_id 并更新热度。 */
int hotness_update_from_path(const char *path)
{
	uint32_t read_id;
	int ret;

	/* 从文件路径解析 read_id。 */
	ret = hotness_parse_read_id(path, &read_id);
	if (ret)
		return ret;

	/* 增加解析得到的 read_id 热度。 */
	return hotness_inc_read_id(read_id);
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

/* 生成 read_id/<id> 形式的目录路径。 */
int hotness_get_read_id_dir(uint32_t read_id, char *out_dir, size_t out_len)
{
	int ret;

	if (!out_dir || !out_len)
		return -EINVAL;

	/* 拼接 read_id 目录路径字符串。 */
	ret = snprintf(out_dir, out_len, READ_ID_DIR_NAME "/%u", read_id);
	if (ret < 0 || (size_t)ret >= out_len)
		return -ENAMETOOLONG;

	return 0;
}

/* 以循环方式分配 read_id 并重置热度。 */
int hotness_alloc_read_id(uint32_t *read_id, char *out_dir, size_t out_len)
{
	uint32_t id;
	int ret;

	if (!hotness_tbl || !read_id)
		return -EINVAL;

	/* 循环分配 read_id。 */
	id = hotness_next_id++ % hotness_max_ids;
	*read_id = id;

	/* 重置已分配 read_id 的热度。 */
	ret = hotness_reset_read_id(id);
	if (ret)
		return ret;

	/* 如需要则为调用方生成目录字符串。 */
	if (out_dir)
		return hotness_get_read_id_dir(id, out_dir, out_len);

	return 0;
}
