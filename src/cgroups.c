#include "cgroups.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * cgroups 设置结构体
 *
 * 用于存储单个 cgroup 控制文件的名称和要写入的值
 * 每个控制文件对应一个资源限制参数（如内存上限、CPU权重等）
 */
struct cgroups_setting {
  char name[CGROUPS_CONTROL_FIELD_SIZE]; // 控制文件的名称（如 "memory.max"）
  char value[CGROUPS_CONTROL_FIELD_SIZE]; // 要写入控制文件的值
};

/**
 * 初始化 cgroups（控制组）配置
 *
 * cgroups 设置通过 cgroups v2 文件系统写入，具体步骤如下：
 * 1. 为新的 cgroup 创建目录（目录名使用容器的主机名）
 * 2. 控制文件会在目录创建时自动生成（由内核自动创建）
 * 3. 将资源限制值写入对应的控制文件中
 *
 * @param hostname 容器主机名，用于创建唯一的 cgroup 目录名
 * @param pid 要添加到 cgroup 中的进程 PID（通常是容器进程的 PID）
 * @return 成功返回 0，失败返回 -1
 */
int cgroups_init(char *hostname, pid_t pid) {
  char cgroup_dir[PATH_MAX] = {0}; // 存储 cgroup 目录的完整路径

  /**
   * "cgroup.procs" 控制文件用于将进程添加到 cgroup 中
   * 这里预先准备好该设置项，使用调用进程的 PID，以便后续将其添加到 cgroup
   *
   * 注意：cgroup.procs 文件写入的是要加入该 cgroup 的进程 PID
   */
  struct cgroups_setting *procs_setting =
      &(struct cgroups_setting){.name = CGROUPS_CGROUP_PROCS, .value = ""};

  // 将 PID 格式化为字符串，存储到 procs_setting 的 value 字段中
  if (snprintf(procs_setting->value, CGROUPS_CONTROL_FIELD_SIZE, "%d", pid) ==
      -1) {
    log_error("failed to setup cgroup.procs setting: %m");
    return -1;
  }

  /**
   * cgroups 允许我们限制进程可以使用的资源，防止容器进程消耗过多系统资源
   * 导致系统上其他服务不可用。
   *
   * 注意：cgroup 必须在进程进入 cgroup 命名空间之前创建。
   *
   * 以下是本容器应用的资源限制：
   * - memory.max: 1GB（进程内存使用上限）
   * - cpu.weight: 256（CPU 时间权重，相当于一个核心的 1/4）
   * - pids.max: 64（容器内可以创建的最大进程/线程数量）
   * - cgroup.procs: <pid>（将指定的进程添加到该 cgroup 中）
   */
  struct cgroups_setting *cgroups_setting_list[] = {
      &(struct cgroups_setting){.name = "memory.max",
                                .value = CGROUPS_MEMORY_MAX}, // 内存限制
      &(struct cgroups_setting){.name = "cpu.weight",
                                .value = CGROUPS_CPU_WEIGHT}, // CPU 权重
      &(struct cgroups_setting){.name = "pids.max",
                                .value = CGROUPS_PIDS_MAX}, // 进程数限制
      procs_setting, // 将容器进程添加到 cgroup 中
      NULL           // 列表结束标记
  };

  log_debug("setting cgroups...");

  // 步骤1：创建 cgroup
  // 目录（snprintf只做字符串拼接，这行代码只是把字符串拼接成类似
  // /sys/fs/cgroup/barcontainer，存储在内存的 cgroup_dir 变量中） cgroups v2
  // 的挂载点在 /sys/fs/cgroup/，每个子目录代表一个 cgroup
  // 使用容器的主机名作为目录名，确保唯一性（可优化）
  if (snprintf(cgroup_dir, sizeof(cgroup_dir), "/sys/fs/cgroup/%s", hostname) ==
      -1) {
    log_error("failed to setup path: %m");
    return -1;
  }

  log_debug("creating %s...", cgroup_dir);
  int fd = open("/sys/fs/cgroup/cgroup.subtree_control", O_WRONLY);
  if (fd >= 0) {
    write(fd, "+cpu +memory +pids", 18);
    close(fd);
  }
  // 创建目录，权限设置为：所有者可读、可写、可执行（0755）
  // 如果目录已存在（上次异常退出残留），先尝试删除再创建
  if (mkdir(cgroup_dir, S_IRUSR | S_IWUSR | S_IXUSR)) {
    if (errno == EEXIST) {
      log_warn("cgroup dir %s already exists, removing and recreating",
               cgroup_dir);
      if (rmdir(cgroup_dir) < 0 && errno != ENOENT) {
        log_error("failed to remove existing cgroup dir %s: %m", cgroup_dir);
        return -1;
      }
      if (mkdir(cgroup_dir, S_IRUSR | S_IWUSR | S_IXUSR)) {
        log_error("failed to mkdir %s: %m", cgroup_dir);
        return -1;
      }
    } else {
      log_error("failed to mkdir %s: %m", cgroup_dir);
      return -1;
    }
  }

  // 步骤2 & 3：遍历所有设置项，将值写入对应的控制文件
  // 内核会在目录创建后自动创建标准的控制文件（如 memory.max, cpu.weight 等）
  for (struct cgroups_setting **setting = cgroups_setting_list; *setting;
       setting++) {
    char setting_dir[PATH_MAX] = {0}; // 控制文件的完整路径
    int fd = 0;                       // 文件描述符

    log_info("setting %s to %s...", (*setting)->name, (*setting)->value);

    // 构建控制文件的完整路径：cgroup目录/控制文件名
    if (snprintf(setting_dir, sizeof(setting_dir), "%s/%s", cgroup_dir,
                 (*setting)->name) == -1) {
      log_error("failed to setup path: %m");
      return -1;
    }

    log_debug("opening %s...", setting_dir);
    // 以只写方式打开控制文件
    if ((fd = open(setting_dir, O_WRONLY)) == -1) {
      log_error("failed to open %s: %m", setting_dir);
      return -1;
    }

    log_debug("writing %s to setting", (*setting)->value);
    // 将设置值写入控制文件（字符串形式）
    if (write(fd, (*setting)->value, strlen((*setting)->value)) == -1) {
      log_error("failed to write %s: %m", setting_dir);
      close(fd);
      return -1;
    }

    log_debug("closing %s...", setting_dir);
    // 关闭文件描述符
    if (close(fd)) {
      log_error("failed to close %s: %m", setting_dir);
      return -1;
    }
  }

  log_debug("cgroups set");
  return 0;
}

/**
 * 清理容器的 cgroups 资源
 *
 * 由于 barco 将子进程的 PID 写入了 cgroup.procs 文件，
 * 当子进程退出后，cgroup 中不再有任何活跃进程，
 * 此时只需要删除 cgroup 目录即可完成清理。
 *
 * 内核会在最后一个进程离开 cgroup 且目录被删除后，
 * 自动释放相关的内核资源。
 *
 * @param hostname 容器主机名，用于定位要删除的 cgroup 目录
 * @return 成功返回 0，失败返回 -1
 */
int cgroups_free(char *hostname) {
  char dir[PATH_MAX] = {0};

  log_debug("freeing cgroups...");

  // 构建 cgroup 目录的完整路径
  if (snprintf(dir, sizeof(dir), "/sys/fs/cgroup/%s", hostname) == -1) {
    log_error("failed to setup paths: %m");
    return -1;
  }

  log_debug("removing %s...", dir);
  // 删除 cgroup 目录（必须是空目录才能删除）
  if (rmdir(dir)) {
    log_error("failed to rmdir %s: %m", dir);
    return -1;
  }

  log_debug("cgroups released");
  return 0;
}