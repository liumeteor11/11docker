#ifndef CGROUPS_H
#define CGROUPS_H

#include <fcntl.h>

/**
 * cgroups（控制组）资源限制相关的常量定义
 *
 * 这些常量定义了容器进程可以使用的系统资源上限，
 * 防止容器消耗过多资源影响宿主机或其他容器
 */

// 容器内存使用上限：1GB
// 当容器内进程尝试分配超过此限制的内存时，将会触发 OOM（内存不足） Killer
#define CGROUPS_MEMORY_MAX "1G"

// CPU 时间权重：256
// 在 cgroups v2 中，cpu.weight 的范围是 1-10000
// 默认值为 100，256 表示约获得 1/4 个 CPU 核心的计算能力
// 这是一个相对权重值，实际分配取决于系统中其他 cgroup 的权重
#define CGROUPS_CPU_WEIGHT "256"

// 容器内允许的最大进程/线程数量：64
// 限制 pids.max 可以防止 fork 炸弹（fork bomb）攻击
// 也防止单个容器创建过多进程导致系统 pid 资源耗尽
#define CGROUPS_PIDS_MAX "64"

// cgroup.procs 控制文件名
// 向此文件写入进程 PID 可将该进程加入到当前 cgroup 中
// 加入后，该进程及其所有子进程都会受到此 cgroup 的资源限制
#define CGROUPS_CGROUP_PROCS "cgroup.procs"

/**
 * cgroups 控制字段的缓冲区大小：256 字节
 *
 * 用于存储控制文件路径或写入值的字符数组大小
 * 足够容纳绝大多数 cgroup 控制文件的路径和参数值
 */
enum { CGROUPS_CONTROL_FIELD_SIZE = 256 };

/**
 * 初始化 cgroups（控制组）配置
 *
 * 为指定的容器（通过主机名标识）创建 cgroup 目录，
 * 并设置内存、CPU、进程数等资源限制。
 *
 * @param hostname 容器主机名，用于创建唯一的 cgroup 目录名
 *                 例如：/sys/fs/cgroup/my_container/
 * @param pid 要添加到 cgroup 中的进程 PID
 *            通常传入容器 init 进程的 PID
 * @return 成功返回 0，失败返回 -1
 */
int cgroups_init(char *hostname, pid_t pid);

/**
 * 清理 cgroups（控制组）资源
 *
 * 删除为容器创建的 cgroup 目录。
 * 必须在容器内所有进程都退出后才能调用，
 * 否则 rmdir 会因为目录非空而失败。
 *
 * @param hostname 容器主机名，用于定位要删除的 cgroup 目录
 * @return 成功返回 0，失败返回 -1
 */
int cgroups_free(char *hostname);

#endif /* CGROUPS_H */