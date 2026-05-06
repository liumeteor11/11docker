#define _GNU_SOURCE 1
#include "user.h"
#include "log.h"
#include <fcntl.h>
#include <grp.h>
#include <linux/limits.h>
#include <sched.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * 初始化用户命名空间（子进程调用）
 *
 * 修改要点：
 * - 增加 unshare(CLONE_NEWUSER) 创建用户命名空间
 * - 移除 setgroups 调用（内核在 gid_map 写入后自动处理组列表）
 * - uid == 0 时跳过所有额外切换（映射后已是 root）
 * - uid != 0 时调用 setresgid/setresuid 切换到目标用户
 */
int user_namespace_init(uid_t uid, int fd) {
  int result = 0;

  log_debug("setting user namespace...");

  if (unshare(CLONE_NEWUSER) < 0) {
    log_error("unshare CLONE_NEWUSER failed: %m");
    return -1;
  }

  log_debug("writing to socket...");
  if (write(fd, &(int){0}, sizeof(int)) != sizeof(int)) {
    log_error("failed to write socket %d: %m", fd);
    return -1;
  }

  log_debug("reading from socket...");
  if (read(fd, &result, sizeof(result)) != sizeof(result)) {
    log_error("failed to read from socket %d: %m", fd);
    return -1;
  }

  if (result) {
    log_error("parent failed to set uid_map/gid_map (code=%d)", result);
    return -1;
  }

  /* 切换到目标 uid/gid。不要调用 setgroups，因为父进程已写入 deny */
  log_debug("switching to uid %d / gid %d...", uid, uid);
  if (setresgid(uid, uid, uid) || setresuid(uid, uid, uid)) {
    log_error("failed to set uid %d / gid %d: %m", uid, uid);
    return -1;
  }

  log_debug("current uid after mapping: %u", getuid());
  log_debug("user namespace set");
  return 0;
}

/**
 * 准备并设置用户命名空间的UID/GID映射（父进程调用）
 *
 * 修改要点：
 * - 增加了 setgroups deny 写入（否则 gid_map 写入失败）
 * - 路径构造增加安全长度检查
 */
int user_namespace_prepare_mappings(pid_t pid, int fd) {
  int map_fd = 0;
  int unshared = -1;

  log_debug("updating uid_map / gid_map...");
  log_debug("retrieving user namespaces status...");
  if (read(fd, &unshared, sizeof(unshared)) != sizeof(unshared)) {
    log_error("failed to retrieve status from socket %d: %m", fd);
    return -1;
  }

  if (!unshared) {
    char dir[PATH_MAX] = {0};

    log_debug("user namespaces enabled");

    /* 0. 禁止 setgroups，否则无法写入 gid_map */
    if (snprintf(dir, sizeof(dir), "/proc/%d/setgroups", pid) >=
        (int)sizeof(dir)) {
      log_error("path too long for setgroups");
      goto fail;
    }
    map_fd = open(dir, O_WRONLY);
    if (map_fd < 0) {
      log_error("failed to open %s: %m", dir);
      goto fail;
    }
    if (write(map_fd, "deny", 4) != 4) {
      log_error("failed to write deny to setgroups: %m");
      close(map_fd);
      goto fail;
    }
    close(map_fd);

    /* 1. 写入 uid_map 和 gid_map */
    log_debug("writing uid_map / gid_map...");
    for (char **file = (char *[]){"uid_map", "gid_map", 0}; *file; file++) {
      if (snprintf(dir, sizeof(dir), "/proc/%d/%s", pid, *file) >=
          (int)sizeof(dir)) {
        log_error("path too long for %s", *file);
        goto fail;
      }

      log_debug("writing %s...", dir);
      if ((map_fd = open(dir, O_WRONLY)) == -1) {
        log_error("failed to open %s: %m", dir);
        goto fail;
      }

      log_debug("writing settings...");
      if (dprintf(map_fd, "%d %d %d\n", USER_NAMESPACE_UID_CHILD_RANGE_START,
                  USER_NAMESPACE_UID_PARENT_RANGE_START,
                  USER_NAMESPACE_UID_CHILD_RANGE_SIZE) == -1) {
        log_error("failed to write %s: %m", *file);
        close(map_fd);
        goto fail;
      }
      close(map_fd);
    }

    log_debug("uid_map and gid_map updated");
  } else {
    log_error("child unshare failed");
    goto fail;
  }

  /* 2. 通知子进程继续 */
  log_debug("updating socket...");
  if (write(fd, &(int){0}, sizeof(int)) != sizeof(int)) {
    log_error("failed to update socket %d: %m", fd);
    return -1;
  }

  return 0;

fail:
  /* 出错时通知子进程，避免其永久阻塞 */
  if (write(fd, &(int){-1}, sizeof(int)) != sizeof(int)) {
    log_error("failed to send error to child");
  }
  return -1;
}