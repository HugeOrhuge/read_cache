#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <f2fs.h>

#include "read_cache.h"
#include "log.h"

static int read_cache_fs_fd = -1;
static size_t packed_zone_threshold_bytes = READ_CACHE_DEFAULT_PACKED_ZONE_BYTES;
static char read_cache_root_dir[PATH_MAX];
#define READ_CACHE_CURSEG_WARM_DATA 1

static const char *rc_strerror(int err)
{
	if (err < 0)
		err = -err;
	return strerror(err);
}

static int read_cache_set_root_dir(const char *root_dir)
{
	size_t len;

	if (!root_dir || !root_dir[0])
		return -EINVAL;

	len = strlen(root_dir);
	if (len >= sizeof(read_cache_root_dir))
		return -ENAMETOOLONG;

	strncpy(read_cache_root_dir, root_dir, sizeof(read_cache_root_dir));
	read_cache_root_dir[sizeof(read_cache_root_dir) - 1] = '\0';
	if (len > 1 && read_cache_root_dir[len - 1] == '/')
		read_cache_root_dir[len - 1] = '\0';

	return 0;
}

static int read_cache_join_root(const char *child, char *out, size_t out_len)
{
	if (!child || !out || !out_len)
		return -EINVAL;
	if (!read_cache_root_dir[0])
		return -ENODEV;

	if (snprintf(out, out_len, "%s/%s", read_cache_root_dir, child)
	    >= (int)out_len)
		return -ENAMETOOLONG;

	return 0;
}

static int read_cache_get_read_id_dir(uint32_t read_id, char *out, size_t out_len)
{
	char read_id_name[PATH_MAX];
	int ret;

	ret = hotness_get_read_id_dir(read_id, read_id_name, sizeof(read_id_name));
	if (ret)
		return ret;

	return read_cache_join_root(read_id_name, out, out_len);
}

/* 初始化热度表和布隆过滤器子系统。 */
int read_cache_init(uint64_t read_id_size_bytes, const char *root_dir)
{
	struct f2fs_free_zone_info info;
	uint64_t zone_bytes;
	uint64_t total_bytes;
	uint64_t max_ids64;
	unsigned int max_ids;
	int ret;

	fs_test_info("init: read_id_size_bytes=%llu\n",
		(unsigned long long)read_id_size_bytes);

	if (!read_id_size_bytes) {
		fs_err("init: invalid read_id_size_bytes\n");
		return -EINVAL;
	}
	ret = read_cache_set_root_dir(root_dir);
	if (ret) {
		fs_err("init: invalid root_dir\n");
		return ret;
	}
	if (read_cache_fs_fd < 0) {
		fs_err("init: fs fd not set\n");
		return -ENODEV;
	}

	ret = ioctl(read_cache_fs_fd, F2FS_IOC_GET_FREE_ZONES, &info);
	if (ret) {
		fs_err("init: get_free_zones ioctl failed: %s\n",
			rc_strerror(errno));
		return -errno;
	}
	if (!info.zone_capacity_blocks) {
		fs_err("init: zone_capacity_blocks is 0\n");
		return -EINVAL;
	}

	zone_bytes = (uint64_t)info.zone_capacity_blocks * READ_CACHE_BLOCK_SIZE;
	if (!zone_bytes) {
		fs_err("init: zone_bytes is 0\n");
		return -EINVAL;
	}

	/* max_ids = free_zone_bytes / read_id_size_bytes - 48 */
	total_bytes = (uint64_t)info.free_zones * zone_bytes;
	max_ids64 = total_bytes / read_id_size_bytes;
	if (max_ids64 <= 48) {
		fs_err("init: not enough space for read_id allocation\n");
		return -ENOSPC;
	}
	max_ids64 -= 48;
	if (max_ids64 > UINT_MAX)
		max_ids64 = UINT_MAX;
	max_ids = (unsigned int)max_ids64;

	ret = hotness_init(max_ids);
	/* 初始化内存热度表。 */
	if (ret) {
		fs_err("init: hotness_init failed: %s\n",
			rc_strerror(ret));
		return ret;
	}

	/* 初始化每个 read_id 的布隆过滤器。 */
	return bloom_filter_init(max_ids, BLOOM_FILTER_BYTES,
				  BLOOM_FILTER_HASHES);
}

struct read_cache_queue_item {
	char *path;
	int fd;
	struct list_head list;
};

static pthread_t read_cache_worker;
static pthread_mutex_t read_cache_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_cache_queue_cond = PTHREAD_COND_INITIALIZER;
static LIST_HEAD(read_cache_queue);
static struct packed_zone read_cache_packed_zone;
static bool read_cache_worker_running;
static bool read_cache_worker_stop;

static void read_cache_pad_cur_zone(const char *read_id_dir);

static void packed_zone_init_if_needed(struct packed_zone *pz)
{
	if (!pz)
		return;
	if (!pz->files.next && !pz->files.prev)
		INIT_LIST_HEAD(&pz->files);
}

/* 设置用于 ioctl 的 f2fs 文件描述符。 */
int read_cache_set_fs_fd(int fd)
{
	if (fd < 0) {
		fs_err("set_fs_fd: invalid fd\n");
		return -EINVAL;
	}

	read_cache_fs_fd = dup(fd);
	fs_test_info("set_fs_fd: fd=%d\n", fd);
	return 0;
}

/* 写入前的设备空间检查占位函数。 */
int read_cache_check_space(const struct packed_zone *pz)
{
	struct f2fs_free_zone_info info;
	int ret;

	if (!pz) {
		fs_err("check_space: pz is NULL\n");
		return -EINVAL;
	}
	if (read_cache_fs_fd < 0) {
		fs_err("check_space: fs fd not set\n");
		return -ENODEV;
	}

	ret = ioctl(read_cache_fs_fd, F2FS_IOC_GET_FREE_ZONES, &info);
	if (ret) {
		fs_err("check_space: get_free_zones ioctl failed: %s\n",
			rc_strerror(errno));
		return -errno;
	}

	return (int)info.free_zones;
}

static void read_cache_queue_push(struct read_cache_queue_item *item)
{
	list_add_tail(&item->list, &read_cache_queue);
}

static struct read_cache_queue_item *read_cache_queue_pop(void)
{
	struct read_cache_queue_item *item;

	if (list_empty(&read_cache_queue))
		return NULL;

	item = list_first_entry(&read_cache_queue,
					struct read_cache_queue_item,
					list);
	list_del(&item->list);
	return item;
}

static void read_cache_queue_clear(void)
{
	struct list_head *pos;
	struct list_head *next;

	list_for_each_safe(pos, next, &read_cache_queue) {
		struct read_cache_queue_item *item;

		item = list_entry(pos, struct read_cache_queue_item, list);
		list_del(&item->list);
		close(item->fd);
		free(item->path);
		free(item);
	}
}

static void *read_cache_worker_main(void *unused)
{
	while (1) {
		struct read_cache_queue_item *item = NULL;
		struct stat st;
		char flushed_dir[PATH_MAX];
		int ret;

		pthread_mutex_lock(&read_cache_queue_lock);
		while (!read_cache_worker_stop && list_empty(&read_cache_queue))
			pthread_cond_wait(&read_cache_queue_cond, &read_cache_queue_lock);
		if (read_cache_worker_stop && list_empty(&read_cache_queue)) {
			pthread_mutex_unlock(&read_cache_queue_lock);
			break;
		}
		item = read_cache_queue_pop();
		pthread_mutex_unlock(&read_cache_queue_lock);

		if (!item)
			continue;

		flushed_dir[0] = '\0';
		if (!fstat(item->fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0) {
			uint64_t size = (uint64_t)st.st_size;
			uint64_t projected = read_cache_packed_zone.total_bytes + size;

			if (read_cache_packed_zone.total_bytes &&
			    projected >= packed_zone_threshold_bytes) {
				fs_test_info("worker: preflush projected=%llu threshold=%llu\n",
					(unsigned long long)projected,
					(unsigned long long)packed_zone_threshold_bytes);
				ret = packed_zone_flush(&read_cache_packed_zone,
							  flushed_dir, sizeof(flushed_dir));
				if (!ret) {
					packed_zone_free(&read_cache_packed_zone);
					read_cache_pad_cur_zone(flushed_dir);
				} else {
					fs_err("worker: flush failed: %s\n",
						rc_strerror(ret));
					close(item->fd);
					free(item->path);
					free(item);
					continue;
				}
			}
		}

		ret = packed_zone_add_file_from_fd(&read_cache_packed_zone,
						    item->path, item->fd);
		close(item->fd);
		free(item->path);
		free(item);

		if (ret) {
			fs_err("worker: add_file failed: %s\n", rc_strerror(ret));
			continue;
		}
	}

	return NULL;
}

/* 设置 packed_zone 刷写阈值（字节）。 */
int read_cache_set_packed_zone_threshold(size_t bytes)
{
	if (!bytes)
		return -EINVAL;

	packed_zone_threshold_bytes = bytes;
	return 0;
}

/* 获取 packed_zone 刷写阈值（字节）。 */
size_t read_cache_get_packed_zone_threshold(void)
{
	return packed_zone_threshold_bytes;
}

/* 启动后台打包线程。 */
int read_cache_start_worker(void)
{
	int ret;

	if (read_cache_worker_running) {
		fs_test_info("start_worker: already running\n");
		return 0;
	}

	read_cache_worker_stop = false;
	read_cache_worker_running = true;
	INIT_LIST_HEAD(&read_cache_queue);
	INIT_LIST_HEAD(&read_cache_packed_zone.files);
	read_cache_packed_zone.total_bytes = 0;

	ret = pthread_create(&read_cache_worker, NULL,
			     read_cache_worker_main, NULL);
	if (ret) {
		read_cache_worker_running = false;
		fs_err("start_worker: pthread_create failed: %s\n",
			rc_strerror(ret));
		return -ret;
	}

	fs_test_info("start_worker: started\n");
	return 0;
}

/* 停止后台打包线程。 */
void read_cache_stop_worker(void)
{
	if (!read_cache_worker_running) {
		fs_test_info("stop_worker: not running\n");
		return;
	}

	pthread_mutex_lock(&read_cache_queue_lock);
	read_cache_worker_stop = true;
	pthread_cond_broadcast(&read_cache_queue_cond);
	pthread_mutex_unlock(&read_cache_queue_lock);

	pthread_join(read_cache_worker, NULL);
	read_cache_worker_running = false;
	fs_test_info("stop_worker: stopped\n");

	packed_zone_free(&read_cache_packed_zone);
	read_cache_queue_clear();
	close(read_cache_fs_fd);
}

/* 入队一个需要打包的文件（由后台线程读取）。 */
int read_cache_enqueue_file(const char *path, int fd)
{
	struct read_cache_queue_item *item;
	char *path_copy;
	int dup_fd;

	if (!path) {
		fs_err("enqueue_file: path is NULL\n");
		return -EINVAL;
	}
	if (fd < 0) {
		fs_err("enqueue_file: invalid fd\n");
		return -EINVAL;
	}
	if (!read_cache_worker_running) {
		fs_err("enqueue_file: worker not running\n");
		return -ENODEV;
	}

	dup_fd = dup(fd);
	if (dup_fd < 0) {
		fs_err("enqueue_file: dup failed: %s\n",
			rc_strerror(errno));
		return -errno;
	}

	path_copy = strdup(path);
	if (!path_copy) {
		fs_err("enqueue_file: strdup failed\n");
		close(dup_fd);
		return -ENOMEM;
	}

	item = calloc(1, sizeof(*item));
	if (!item) {
		fs_err("enqueue_file: alloc failed\n");
		free(path_copy);
		close(dup_fd);
		return -ENOMEM;
	}

	item->path = path_copy;
	item->fd = dup_fd;
	INIT_LIST_HEAD(&item->list);

	pthread_mutex_lock(&read_cache_queue_lock);
	read_cache_queue_push(item);
	pthread_cond_signal(&read_cache_queue_cond);
	pthread_mutex_unlock(&read_cache_queue_lock);
	fs_test_info("enqueue_file: queued path=%s\n", path);

	return 0;
}

/* 判断 packed_zone 是否满足刷写阈值。 */
int packed_zone_should_flush(const struct packed_zone *pz)
{
	if (!pz)
		return -EINVAL;

	return pz->total_bytes >= packed_zone_threshold_bytes;
}

/* 释放 packed_zone 中的所有文件数据。 */
void packed_zone_free(struct packed_zone *pz)
{
	struct list_head *pos;
	struct list_head *next;

	if (!pz)
		return;

	packed_zone_init_if_needed(pz);

	list_for_each_safe(pos, next, &pz->files) {
		struct packed_file *file = list_entry(pos, struct packed_file, list);
		list_del(&file->list);
		free((void *)file->path);
		free((void *)file->data);
		free(file);
	}

	INIT_LIST_HEAD(&pz->files);
	pz->total_bytes = 0;
}

/* 将文件读入内存并追加到 packed_zone。 */
int packed_zone_add_file_from_fd(struct packed_zone *pz, const char *path, int fd)
{
	struct packed_file *file;
	struct stat st;
	char *path_copy;
	void *buf = NULL;
	uint64_t size;
	off_t offset = 0;
	int ret;

	if (!pz || !path) {
		fs_err("add_file: invalid args\n");
		return -EINVAL;
	}
	packed_zone_init_if_needed(pz);
	if (fd < 0) {
		fs_err("add_file: invalid fd\n");
		return -EINVAL;
	}
	if (fstat(fd, &st)) {
		fs_err("add_file: fstat failed: %s\n",
			rc_strerror(errno));
		return -errno;
	}
	if (!S_ISREG(st.st_mode)) {
		fs_err("add_file: not a regular file\n");
		return -EINVAL;
	}
	if (st.st_size < 0) {
		fs_err("add_file: negative size\n");
		return -EINVAL;
	}
	if ((uint64_t)st.st_size > (uint64_t)LLONG_MAX) {
		fs_err("add_file: size exceeds LLONG_MAX\n");
		return -EFBIG;
	}
	if ((uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
		fs_err("add_file: size exceeds SIZE_MAX\n");
		return -EFBIG;
	}

	size = (uint64_t)st.st_size;
	if (size) {
		buf = malloc(size);
		if (!buf)
			return -ENOMEM;

		while (size) {
			size_t chunk = size;
			ssize_t read_bytes;

			if (chunk > (size_t)SSIZE_MAX)
				chunk = (size_t)SSIZE_MAX;

			read_bytes = pread(fd, (char *)buf + (size_t)offset, chunk, offset);
			if (read_bytes < 0) {
				if (errno == EINTR)
					continue;
				fs_err("add_file: read failed: %s\n",
					rc_strerror(errno));
				ret = -errno;
				goto out_free;
			}
			if (read_bytes == 0) {
				fs_err("add_file: unexpected EOF\n");
				ret = -EIO;
				goto out_free;
			}

			offset += read_bytes;
			size -= (uint64_t)read_bytes;
		}
	}

	path_copy = strdup(path);
	if (!path_copy) {
		fs_err("add_file: strdup failed\n");
		ret = -ENOMEM;
		goto out_free;
	}

	file = calloc(1, sizeof(*file));
	if (!file) {
		fs_err("add_file: alloc failed\n");
		free(path_copy);
		ret = -ENOMEM;
		goto out_free;
	}

	file->path = path_copy;
	file->data = buf;
	file->size = (size_t)st.st_size;
	INIT_LIST_HEAD(&file->list);
	list_add(&file->list, &pz->files);
	pz->total_bytes += file->size;

	return 0;

out_free:
	free(buf);
	return ret;
}

/* 递归创建目录，行为类似 mkdir -p。 */
static int read_cache_mkdir_p(const char *path, mode_t mode)
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
static int read_cache_ensure_parent_dirs(const char *path, mode_t mode)
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
	return read_cache_mkdir_p(tmp, mode);
}

/* 递归删除所有文件和子目录。 */
static int read_cache_unlink_dir_recursive(const char *dir_path)
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
			ret = read_cache_unlink_dir_recursive(path);
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
	return read_cache_unlink_dir_recursive(read_id_dir);
}

/* 将内存缓冲区写入文件路径。 */
static int read_cache_write_file(const char *path, const void *data, size_t size)
{
	int fd;
	ssize_t written;
	const char *buf = data;
	int ret;

	/* 创建文件对应的目录链。 */
	ret = read_cache_ensure_parent_dirs(path, 0755);
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

static int read_cache_write_zeros(const char *path, size_t size)
{
	int fd;
	static char zeros[4096];
	int ret;

	if (!size)
		return 0;

	ret = read_cache_ensure_parent_dirs(path, 0755);
	if (ret)
		return ret;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -errno;

	while (size) {
		size_t chunk = sizeof(zeros);
		ssize_t written;

		if (chunk > size)
			chunk = size;

		written = write(fd, zeros, chunk);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			ret = -errno;
			goto out;
		}
		size -= (size_t)written;
	}

	ret = 0;
out:
	close(fd);
	return ret;
}

static void read_cache_pad_cur_zone(const char *read_id_dir)
{
	struct f2fs_cursec_free_info info = { 0 };
	char pad_path[PATH_MAX];
	size_t pad_bytes;

	if (!read_id_dir || !read_id_dir[0])
		return;
	if (read_cache_fs_fd < 0)
		return;

	info.curseg_type = READ_CACHE_CURSEG_WARM_DATA;
	if (ioctl(read_cache_fs_fd, F2FS_IOC_GET_CURSEC_FREE_BLOCKS, &info)) {
		fs_err("pad_cur_zone: get_cursec_free_blocks failed: %s\n",
			rc_strerror(errno));
		return;
	}

	pad_bytes = (size_t)info.free_blocks * READ_CACHE_BLOCK_SIZE;
	if (!pad_bytes)
		return;

	if (snprintf(pad_path, sizeof(pad_path), "%s/.pad", read_id_dir)
	    >= (int)sizeof(pad_path))
		return;

	fs_test_info("pad_cur_zone: pad_bytes=%zu\n", pad_bytes);
	read_cache_write_zeros(pad_path, pad_bytes);
}

/* 将 packed zone 写入新的 read_id 目录。 */
int packed_zone_flush(struct packed_zone *pz, char *out_dir, size_t out_len)
{
	struct packed_file *file;
	struct list_head *pos;
	char evict_dir[PATH_MAX];
	char read_id_dir[PATH_MAX];
	char read_id_name[PATH_MAX];
	char file_path[PATH_MAX];
	uint32_t evict_id;
	uint32_t read_id;
	int ret;

	if (!pz) {
		fs_err("flush: pz is NULL\n");
		return -EINVAL;
	}

	packed_zone_init_if_needed(pz);

	/* 写入前检查空间，不足则驱逐热度最小的 read_id。 */
	while (read_cache_check_space(pz) <= 0) {
		/* 取出热度最小的 read_id。 */
		ret = hotness_get_min_read_id(&evict_id);
		if (ret == -ENOENT)
			return -ENOSPC;
		if (ret < 0)
			return ret;
		fs_test_info("flush: evict read_id=%u\n", evict_id);

		/* 生成被驱逐 read_id 的目录名。 */
		ret = read_cache_get_read_id_dir(evict_id, evict_dir, sizeof(evict_dir));
		if (ret)
			return ret;

		/* 删除该 read_id 目录。 */
		ret = read_cache_unlink_read_id_dir(evict_dir);
		if (ret && ret != -ENOENT)
			return ret;

		/* 重置被驱逐 read_id 的布隆过滤器位图。 */
		bloom_filter_reset_read_id(evict_id);
		/* 标记该 read_id 为未使用。 */
		hotness_release_read_id(evict_id);
	}

	/* 空间足够后，分配新的 read_id 并生成目录名。 */
	ret = hotness_alloc_read_id(&read_id, read_id_name,
				    sizeof(read_id_name));
	if (ret)
		return ret;
	ret = read_cache_join_root(read_id_name, read_id_dir, sizeof(read_id_dir));
	if (ret)
		return ret;
	fs_test_info("flush: allocated read_id=%u\n", read_id);

	/* 清理该 read_id 旧数据。 */
	read_cache_unlink_read_id_dir(read_id_dir);
	/* 创建 read_id 顶层目录。 */
	ret = read_cache_mkdir_p(read_id_dir, 0755);
	if (ret) {
		fs_err("flush: mkdir_p failed: %s\n", rc_strerror(ret));
		return ret;
	}

	if (out_dir && out_len) {
		int copied = snprintf(out_dir, out_len, "%s", read_id_dir);
		if (copied < 0 || (size_t)copied >= out_len)
			return -ENAMETOOLONG;
	}

	/* 重置该 read_id 的布隆过滤器位图。 */
	bloom_filter_reset_read_id(read_id);

	list_for_each(pos, &pz->files) {
		file = list_entry(pos, struct packed_file, list);
		/* 生成 read_id 目录下的完整路径。 */
		if (snprintf(file_path, sizeof(file_path), "%s/%s",
			     read_id_dir, file->path) >= (int)sizeof(file_path))
			{
				ret = -ENAMETOOLONG;
				goto out_release;
			}

		/* 写入文件内容到f2fs */
		ret = read_cache_write_file(file_path, file->data, file->size);
		if (ret) {
			fs_err("flush: write failed: %s\n",
				rc_strerror(ret));
			goto out_release;
		}

		/* 将路径写入该 read_id 的布隆过滤器 */
		bloom_filter_set(read_id, file->path);
		/* 统计该 read_id 的热度 */
		hotness_inc_read_id(read_id);
	}

	/* 记录该 read_id 为最新写入。 */
	ret = hotness_mark_written(read_id);
	if (ret)
		goto out_release;

	return 0;

out_release:
	read_cache_unlink_read_id_dir(read_id_dir);
	bloom_filter_reset_read_id(read_id);
	hotness_release_read_id(read_id);
	return ret;
}

/* 尝试在指定 read_id 目录下打开并验证文件。 */
static int read_cache_try_read(uint32_t read_id, const char *file_path)
{
	char full_path[PATH_MAX];
	char read_id_dir[PATH_MAX];
	int fd;
	char buf;

	if (read_cache_get_read_id_dir(read_id, read_id_dir, sizeof(read_id_dir)))
		return -ENODEV;
	if (snprintf(full_path, sizeof(full_path),
		     "%s/%s", read_id_dir, file_path)
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
	LIST_HEAD(candidates);
	struct list_head *pos;
	struct read_id_list *cur;
	int ret;
	int fd;

	if (!file_path) {
		fs_err("query: file_path is NULL\n");
		return -EINVAL;
	}

	/* 通过布隆过滤器查询候选 read_id。 */
	ret = bloom_filter_query(file_path, &candidates);
	if (ret < 0) {
		fs_err("query: bloom_filter_query failed: %s\n",
			rc_strerror(ret));
		return ret;
	}
	if (ret == 0) {
		fs_test_info("query: miss path=%s\n", file_path);
		return -ENOENT;
	}

	list_for_each(pos, &candidates) {
		cur = list_entry(pos, struct read_id_list, list);
		/* 依次在候选 read_id 下尝试打开文件。 */
		fd = read_cache_try_read(cur->id, file_path);
		if (fd == -ENAMETOOLONG) {
			/* 路径错误时先释放候选列表。 */
			bloom_filter_free_list(&candidates);
			return fd;
		}
		if (fd >= 0) {
			/* 命中后释放候选列表并返回 fd。 */
			bloom_filter_free_list(&candidates);
			fs_test_info("query: hit path=%s\n", file_path);
			return fd;
		}
	}

	/* 遍历结束后释放候选列表。 */
	bloom_filter_free_list(&candidates);
	fs_test_info("query: miss path=%s\n", file_path);
	return -ENOENT;
}
