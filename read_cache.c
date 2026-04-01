#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "read_cache.h"

/* 初始化热度表和布隆过滤器子系统。 */
int read_cache_init(void)
{
	int ret;

	ret = hotness_init(READ_ID_MAX);
	/* 初始化内存热度表。 */
	if (ret)
		return ret;

	/* 初始化每个 read_id 的布隆过滤器。 */
	return bloom_filter_init(READ_ID_MAX, BLOOM_FILTER_BYTES,
				  BLOOM_FILTER_HASHES);
}

/* 写入前的设备空间检查占位函数。 */
int read_cache_check_space(const struct packed_zone *pz)
{
	if (!pz)
		return -EINVAL;

	/* TODO: implement device space and zone checks. */
	return 1;
}

/* 递归创建目录，行为类似 mkdir -p。 */
static int mkdir_p(const char *path, mode_t mode)
{
	char tmp[PATH_MAX];
	char *p;

	if (!path)
		return -EINVAL;
	if (strlen(path) >= sizeof(tmp))
		return -ENAMETOOLONG;

	/* 逐段创建目录。 */
	strncpy(tmp, path, sizeof(tmp));
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		/* 创建中间目录。 */
		if (mkdir(tmp, mode) && errno != EEXIST)
			return -errno;
		*p = '/';
	}

	/* 创建最终目录。 */
	if (mkdir(tmp, mode) && errno != EEXIST)
		return -errno;

	return 0;
}

/* 确保文件路径的父目录链存在。 */
static int ensure_parent_dirs(const char *path, mode_t mode)
{
	char tmp[PATH_MAX];
	char *slash;

	if (!path)
		return -EINVAL;
	if (strlen(path) >= sizeof(tmp))
		return -ENAMETOOLONG;

	strncpy(tmp, path, sizeof(tmp));
	slash = strrchr(tmp, '/');
	if (!slash)
		return 0;

	*slash = '\0';
	/* 确保父目录树存在。 */
	return mkdir_p(tmp, mode);
}

/* 递归删除所有文件和子目录。 */
static int unlink_dir_recursive(const char *dir_path)
{
	DIR *dir;
	struct dirent *ent;
	char path[PATH_MAX];
	struct stat st;
	int ret = 0;

	if (!dir_path)
		return -EINVAL;

	dir = opendir(dir_path);
	if (!dir)
		return -errno;

	while ((ent = readdir(dir))) {
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;

		if (snprintf(path, sizeof(path), "%s/%s", dir_path,
			     ent->d_name) >= (int)sizeof(path)) {
			ret = -ENAMETOOLONG;
			break;
		}

		if (lstat(path, &st)) {
			ret = -errno;
			break;
		}

		if (S_ISDIR(st.st_mode))
			/* 递归进入子目录。 */
			ret = unlink_dir_recursive(path);
		else
			/* 删除普通文件或符号链接。 */
			ret = unlink(path) ? -errno : 0;

		if (ret)
			break;
	}

	closedir(dir);
	if (!ret)
		/* 删除已清空的目录。 */
		ret = rmdir(dir_path);

	return ret;
}

/* 删除 read_id 目录及其全部内容。 */
int read_cache_unlink_read_id_dir(const char *read_id_dir)
{
	struct stat st;

	if (!read_id_dir)
		return -EINVAL;
	if (lstat(read_id_dir, &st))
		return -errno;
	if (!S_ISDIR(st.st_mode))
		return -ENOTDIR;

	/* 递归删除目录内容。 */
	return unlink_dir_recursive(read_id_dir);
}

/* 将内存缓冲区写入文件路径。 */
static int write_file(const char *path, const void *data, size_t size)
{
	int fd;
	ssize_t written;
	const char *buf = data;
	int ret;

	/* 创建文件对应的目录链。 */
	ret = ensure_parent_dirs(path, 0755);
	if (ret)
		return ret;

	/* 打开并截断目标文件。 */
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -errno;

	while (size) {
		/* 处理 EINTR 重试写入剩余数据。 */
		written = write(fd, buf, size);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			ret = -errno;
			goto out;
		}
		buf += written;
		size -= written;
	}

	ret = 0;
out:
	close(fd);
	return ret;
}

/* 将 packed zone 写入新的 read_id 目录。 */
int packed_zone_flush(struct packed_zone *pz)
{
	struct packed_file *file;
	char read_id_dir[PATH_MAX];
	char file_path[PATH_MAX];
	uint32_t read_id;
	int ret;

	if (!pz)
		return -EINVAL;

	/* 写入前检查空间。 */
	ret = read_cache_check_space(pz);
	if (ret <= 0)
		return -ENOSPC;

	/* 分配 read_id 并生成目录名。 */
	ret = hotness_alloc_read_id(&read_id, read_id_dir,
				    sizeof(read_id_dir));
	if (ret)
		return ret;

	/* 清理该 read_id 旧数据。 */
	read_cache_unlink_read_id_dir(read_id_dir);
	/* 创建 read_id 顶层目录。 */
	ret = mkdir_p(read_id_dir, 0755);
	if (ret)
		return ret;

	/* 重置该 read_id 的布隆过滤器位图。 */
	bloom_filter_reset_read_id(read_id);

	for (file = pz->files; file; file = file->next) {
		/* 生成 read_id 目录下的完整路径。 */
		if (snprintf(file_path, sizeof(file_path), "%s/%s",
			     read_id_dir, file->path) >= (int)sizeof(file_path))
			return -ENAMETOOLONG;

		/* 写入文件内容到存储。 */
		ret = write_file(file_path, file->data, file->size);
		if (ret)
			return ret;

		/* 将路径写入该 read_id 的布隆过滤器。 */
		bloom_filter_set(read_id, file->path);
		/* 统计该路径对应 read_id 的热度。 */
		hotness_update_from_path(file_path);
	}

	return 0;
}

/* 尝试在指定 read_id 目录下打开并验证文件。 */
static int read_cache_try_read(uint32_t read_id, const char *file_path)
{
	char full_path[PATH_MAX];
	int fd;
	char buf;

	if (snprintf(full_path, sizeof(full_path),
		     READ_ID_DIR_NAME "/%u/%s", read_id, file_path)
	    >= (int)sizeof(full_path))
		return -ENAMETOOLONG;

	fd = open(full_path, O_RDONLY);
	if (fd < 0)
		return -errno;

	/* 读取一个字节以验证文件存在且可读。 */
	if (read(fd, &buf, sizeof(buf)) < 0) {
		close(fd);
		return -errno;
	}

	/* 重置文件偏移，便于调用方继续读取。 */
	lseek(fd, 0, SEEK_SET);
	/* 增加命中的 read_id 热度。 */
	hotness_inc_read_id(read_id);

	return fd;
}

/* 查询缓存路径，命中则返回打开的 fd。 */
int read_cache_query(const char *file_path)
{
	struct read_id_list *candidates = NULL;
	struct read_id_list *cur;
	int ret;
	int fd;

	if (!file_path)
		return -EINVAL;

	/* 通过布隆过滤器查询候选 read_id。 */
	ret = bloom_filter_query(file_path, &candidates);
	if (ret < 0)
		return ret;
	if (ret == 0)
		return -ENOENT;

	for (cur = candidates; cur; cur = cur->next) {
		/* 依次在候选 read_id 下尝试打开文件。 */
		fd = read_cache_try_read(cur->id, file_path);
		if (fd == -ENAMETOOLONG) {
			/* 路径错误时先释放候选列表。 */
			bloom_filter_free_list(candidates);
			return fd;
		}
		if (fd >= 0) {
			/* 命中后释放候选列表并返回 fd。 */
			bloom_filter_free_list(candidates);
			return fd;
		}
	}

	/* 遍历结束后释放候选列表。 */
	bloom_filter_free_list(candidates);
	return -ENOENT;
}
