## 写
1. 先检查设备上有没有足够大的空间(怎么判断？)
2. 新增 unlink read_id 目录函数。遍历 read_id 目录，将目录下所有文件都unlink掉。
3. 新增 packed_zone_flush(struct * packed_zone)
  a. 检查剩余 zone 的数量，如果没有足够的数量，则挑选最早的 read_id，并将对应目录下所有文件unlink。
  b. 将read_id ++，创建新的 read_id 目录
  c. 遍历传入的 packed_zone，将 packed_zone 中的每个缓存文件通过调用 write 系统调用写入到 f2fs 的 read_id 目录。
  d. 初始化 read_id 加入到 read_id 链表
  e. 调用热度更新函数和布隆过滤器设置函数

## 读
1. 新增查询 read_id 函数
  a. 通过查布隆过滤器，得到可能的 read_id 组
  b. 如果得到的 read_id 组为空，返回空
  c. 不为空则遍历 read_id 组，查看该每个 read_id 目录下有没有对应文件路径的文件，调用 read 系统调用读文件，然后找到文件，同时调用热度增加函数，将 read_id 的访问次数+1。返回文件描述符。

## hotness table 相关函数
热度表维护在内存中，记录每个read_id的总访问次数。文件组织成read_id/xx 形式，需要统计的是对应read_id的热度。主要涉及以下函数：
1. init 函数，初始化 read_id 链表，此链表具备最大长度，达到最大长度需要循环使用 id。
2. reset 函数来重置传入 read_id 的总热度
3. read_id热度更新函数，文件每被访问一次，就通过解析文件路径中的开头 read_id 来自增 read_id访问次数。
4. 查询最小热度 read_id 函数。

## bloom filter 相关函数
维护的是文件可能在哪个 read_id 上的布隆过滤器。
1. 布隆过滤器 set 函数，设置对应 read_id 的布隆过滤器 bit 位。
2. 布隆过滤器 reset 函数
3. 布隆过滤器init函数
4. 布隆过滤器查询可能 read_id 组函数，返回一个可能 read_id 的链表。