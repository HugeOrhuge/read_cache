#ifndef HOTNESS_H
#define HOTNESS_H

#include <stddef.h>
#include <stdint.h>

int hotness_init(unsigned int max_read_ids);
/* 重置指定 read_id 的热度计数。 */
int hotness_reset_read_id(uint32_t read_id);
/* 从路径解析 read_id 并增加热度。 */
int hotness_update_from_path(const char *path);
/* 增加指定 read_id 的热度。 */
int hotness_inc_read_id(uint32_t read_id);
/* 查找热度最小的 read_id。 */
int hotness_get_min_read_id(uint32_t *read_id);
/* 以循环方式分配新的 read_id。 */
int hotness_alloc_read_id(uint32_t *read_id, char *out_dir, size_t out_len);
/* 生成 read_id 目录路径到 out_dir。 */
int hotness_get_read_id_dir(uint32_t read_id, char *out_dir, size_t out_len);
/* 判断 read_id 是否正在使用。 */
int hotness_is_used(uint32_t read_id);

#endif
