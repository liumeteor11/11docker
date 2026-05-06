#ifndef USER_H
#define USER_H

#include <sys/types.h>

/**
 * 用户命名空间（User Namespace）配置常量
 *
 * 用户命名空间是 Linux 命名空间的一种，用于隔离用户和组 ID。
 * 进程可以在其用户命名空间中拥有 root 权限，而不影响宿主机。
 * 这些常量定义了 UID 映射的范围和偏移量。
 */
enum {
  // 用户命名空间设置

  /**
   * 父命名空间中 UID 范围的起始值
   *
   * 表示在宿主机（父命名空间）中，子命名空间的 UID 将映射到从 0
   * 开始的这个偏移量。 这里设置为 0，意味着子命名空间中的 root（UID
   * 0）将映射到父命名空间中的 UID 0， 但实际映射关系需要通过 /proc/self/uid_map
   * 文件来配置。
   */
  USER_NAMESPACE_UID_PARENT_RANGE_START = 10000,

  /**
   * 子命名空间中 UID 范围的起始值
   *
   * 定义在子用户命名空间内部可用的起始 UID。
   * 这里设置为 10000，意味着子命名空间中第一个可用的 UID 是 10000，
   * 通常子命名空间中的 root（UID 0）会映射到父命名空间中的某个非特权 UID。
   *
   * 选择 10000 作为起始值可以避免与系统保留的 UID（通常 < 1000）冲突，
   * 也留出了空间给系统账户和普通用户。
   */
  USER_NAMESPACE_UID_CHILD_RANGE_START = 0,

  /**
   * 子命名空间中可用的 UID 范围大小
   *
   * 定义子用户命名空间可以映射的连续 UID 数量。
   * 这里设置为 2000，意味着可以映射从 10000 到 11999 共 2000 个 UID。
   *
   * 这个范围需要足够大，以满足容器或沙箱内多用户场景的需求，
   * 同时也不能太大，以免耗尽宿主机上的可用 UID。
   */
  USER_NAMESPACE_UID_CHILD_RANGE_SIZE = 1,
};

/**
 * 为进程初始化用户命名空间
 *
 * 该函数创建一个新的用户命名空间，并将调用进程加入其中。
 * 在新的用户命名空间中，进程可以获得该命名空间内的 root 权限，
 * 但宿主机上仍然是一个普通用户，从而实现特权隔离。
 *
 * 典型使用场景：
 *   - 容器运行时（如 Docker、Podman）
 *   - 特权降级的沙箱环境
 *   - 需要部分 root 权限但又不想完全信任的进程
 *
 * @param uid  目标用户 ID，用于设置命名空间中的初始用户身份
 * @param fd   文件描述符，通常指向 /proc/self/uid_map 或类似文件，
 *             用于后续的用户 ID 映射配置
 *
 * @return 成功返回 0，失败返回 -1 并设置 errno
 */
int user_namespace_init(uid_t uid, int fd);

/**
 * 配置命名空间的用户和组 ID 映射关系
 *
 * 该函数负责为子进程配置用户命名空间中的 UID 和 GID 映射。
 * 映射关系通过写入 /proc/[pid]/uid_map 和 /proc/[pid]/setgroups 文件来实现，
 * 允许子命名空间中的 UID 映射到父命名空间（宿主机）中的不同 UID。
 * 这里的 [pid] 就是目标命名空间内任意一个进程 的
 * PID（通常是该命名空间的第一个进程),用户命名空间是进程属性
 *
 * 映射示例：
 *   子命名空间 UID 0（root） -> 宿主机 UID 10000
 *   子命名空间 UID 1         -> 宿主机 UID 10001
 *   以此类推...
 *
 * 在配置映射之前，通常需要先禁用 setgroups 权限（写入 /proc/[pid]/setgroups），
 * 以防止子进程提升权限。这是 Linux 内核的安全要求。
 *
 * @param pid  子进程的 PID，用于定位其 /proc/[pid]/ 目录下的映射文件
 * @param fd   文件描述符，通常指向 /proc/self/gid_map 或类似文件，
 *             用于组 ID 映射配置
 *
 * @note 调用此函数时，子进程应该已经创建但尚未执行用户代码，
 *       通常是在 fork() 之后、exec() 之前调用。
 *
 * @return 成功返回 0，失败返回 -1 并设置 errno
 */
int user_namespace_prepare_mappings(pid_t pid, int fd);

#endif /* USER_H */