#define _POSIX_C_SOURCE 199309L
#include "argtable3.h"
#include "cgroups.h"
#include "container.h"
#include "log.h"
#include "netlink.h"
#include "user.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 命令行参数最大数量 */
enum { ARGTABLE_ARG_MAX = 20 };

/* ========== argtable 全局变量定义 ========== */
struct arg_lit *help, *version;
struct arg_int *uid;
struct arg_str *mnt, *cmd, *arg;
struct arg_lit *vrb;

/* 网络相关参数 */
struct arg_str *veth_name;
struct arg_str *veth_peer;
struct arg_str *container_ip;
struct arg_str *host_ip;

/* 新增：NAT 和 DNS 参数 */
struct arg_lit *nat;
struct arg_str *dns;

struct arg_end *end;

/* ========== 辅助函数：生成随机后缀 ========== */
static void generate_random_suffix(char *buf, size_t len) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  unsigned int seed = (unsigned int)(ts.tv_nsec ^ ts.tv_sec ^ getpid());
  srand(seed);

  for (size_t i = 0; i < len - 1 && i < 8; i++) {
    buf[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[rand() % 36];
  }
  buf[len - 1] = '\0';
}

/* ========== 辅助函数：启用 IP 转发 ========== */
static int enable_ip_forward(void) {
  const char *path = "/proc/sys/net/ipv4/ip_forward";
  int fd = open(path, O_WRONLY);
  if (fd < 0) {
    log_warn("cannot open %s for writing", path);
    return -1;
  }
  ssize_t n = write(fd, "1\n", 2);
  close(fd);
  return (n == 2) ? 0 : -1;
}

/* ========== 辅助函数：自动检测宿主机出口接口 ========== */
static int detect_host_interface(char *buf, size_t len) {
  FILE *fp = popen(
      "ip route show default 2>/dev/null | awk '{print $5}' | head -1", "r");
  if (!fp) {
    return -1;
  }
  if (fgets(buf, len, fp) == NULL) {
    pclose(fp);
    return -1;
  }
  pclose(fp);

  /* 去掉尾部换行 */
  size_t slen = strlen(buf);
  while (slen > 0 && (buf[slen - 1] == '\n' || buf[slen - 1] == '\r')) {
    buf[--slen] = '\0';
  }
  return (slen > 0) ? 0 : -1;
}

/* ========== 辅助函数：配置 NAT 规则 ========== */
static int setup_nat(const char *subnet, const char *host_if) {
  char cmd[512];

  log_info("setting up NAT: %s -> %s (masquerade)", subnet, host_if);

  /* 添加 MASQUERADE 规则（幂等：先检查再添加） */
  snprintf(cmd, sizeof(cmd),
           "iptables -t nat -C POSTROUTING -s %s -o %s -j MASQUERADE "
           "2>/dev/null || "
           "iptables -t nat -A POSTROUTING -s %s -o %s -j MASQUERADE",
           subnet, host_if, subnet, host_if);
  if (system(cmd) != 0) {
    log_warn("failed to add NAT MASQUERADE rule");
    return -1;
  }

  /* 允许从容器子网发出的转发 */
  snprintf(cmd, sizeof(cmd),
           "iptables -C FORWARD -s %s -j ACCEPT "
           "2>/dev/null || "
           "iptables -A FORWARD -s %s -j ACCEPT",
           subnet, subnet);
  if (system(cmd) != 0) {
    log_warn("failed to add FORWARD ACCEPT rule for source");
  }

  /* 允许已建立连接的返回流量 */
  snprintf(cmd, sizeof(cmd),
           "iptables -C FORWARD -d %s -m state --state RELATED,ESTABLISHED "
           "-j ACCEPT 2>/dev/null || "
           "iptables -A FORWARD -d %s -m state --state RELATED,ESTABLISHED "
           "-j ACCEPT",
           subnet, subnet);
  if (system(cmd) != 0) {
    log_warn("failed to add FORWARD ESTABLISHED rule");
  }

  log_info("NAT rules configured");
  return 0;
}

/* ========== 辅助函数：清理 NAT 规则 ========== */
static void cleanup_nat(const char *subnet, const char *host_if) {
  char cmd[512];

  log_info("removing NAT rules for %s", subnet);

  snprintf(cmd, sizeof(cmd),
           "iptables -t nat -D POSTROUTING -s %s -o %s -j MASQUERADE "
           "2>/dev/null",
           subnet, host_if);
  system(cmd);

  snprintf(cmd, sizeof(cmd), "iptables -D FORWARD -s %s -j ACCEPT 2>/dev/null",
           subnet);
  system(cmd);

  snprintf(cmd, sizeof(cmd),
           "iptables -D FORWARD -d %s -m state --state RELATED,ESTABLISHED "
           "-j ACCEPT 2>/dev/null",
           subnet);
  system(cmd);
}

/* ========== 主函数 ========== */
int main(int argc, char **argv) {
  char *stack = NULL;
  container_config *config = NULL;
  int container_pid = -1;
  int sockets[2] = {-1, -1};
  int exitcode = 0;
  int container_status = 0;
  char progname[] = "barco";
  int veth_created = 0;
  int nat_applied = 0;
  char host_if[64] = {0};

  /* 堆分配 config */
  config = calloc(1, sizeof(container_config));
  if (!config) {
    fprintf(stderr, "failed to allocate config: %m\n");
    return 1;
  }

  /* ========== 1. 初始化命令行参数表 ========== */
  void *argtable[] = {
      help = arg_litn(NULL, "help", 0, 1, "display help and exit"),
      version = arg_litn(NULL, "version", 0, 1, "display version and exit"),

      uid = arg_intn("u", "uid", "<n>", 1, 1, "UID/GID inside container"),
      mnt = arg_strn("m", "mnt", "<path>", 1, 1, "rootfs directory"),
      cmd = arg_strn("c", "cmd", "<cmd>", 1, 1, "command to execute"),
      arg = arg_strn("a", "arg", "<arg>", 0, 1, "argument for command"),

      vrb = arg_litn("v", "verbosity", 0, 1, "verbose output"),

      veth_name = arg_strn(NULL, "veth", "<name>", 0, 1,
                           "veth name inside container (e.g., eth0)"),
      veth_peer =
          arg_strn(NULL, "veth-peer", "<name>", 0, 1,
                   "veth peer name on host (auto-generated if omitted)"),
      container_ip = arg_strn(NULL, "container-ip", "<ip/cidr>", 0, 1,
                              "IP address for container (e.g., 10.0.0.2/24)"),
      host_ip = arg_strn(NULL, "host-ip", "<ip/cidr>", 0, 1,
                         "IP address for host veth (e.g., 10.0.0.1/24)"),

      /* 新增参数 */
      nat = arg_litn(NULL, "nat", 0, 1,
                     "enable NAT/masquerade for container internet access"),
      dns = arg_strn(NULL, "dns", "<ip>", 0, 1,
                     "DNS server for container (default: 8.8.8.8)"),

      end = arg_end(ARGTABLE_ARG_MAX),
  };

  /* ========== 2. 解析命令行参数 ========== */
  if (arg_parse(argc, argv, argtable) > 0) {
    arg_print_errors(stderr, end, progname);
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
    exitcode = 1;
    goto exit;
  }

  if (help->count > 0) {
    printf("Usage: %s", progname);
    arg_print_syntax(stdout, argtable, "\n");
    arg_print_glossary(stdout, argtable, "  %-20s %s\n");
    goto exit;
  }

  log_set_level(vrb->count > 0 ? LOG_TRACE : LOG_INFO);

  /* ========== 3. 填充容器配置 ========== */
  config->hostname = "barcontainer";
  config->uid = uid->ival[0];
  config->mnt = (char *)mnt->sval[0];
  config->cmd = (char *)cmd->sval[0];
  config->arg = arg->count ? (char *)arg->sval[0] : NULL;

  /* DNS 配置 */
  if (dns->count > 0) {
    config->dns_server = (char *)dns->sval[0];
  } else {
    config->dns_server = "8.8.8.8";
  }

  /* 网络配置 */
  if (veth_name->count > 0 && container_ip->count > 0) {
    config->veth_name = (char *)veth_name->sval[0];
    config->container_ip = (char *)container_ip->sval[0];
    config->host_ip = host_ip->count ? (char *)host_ip->sval[0] : NULL;
    config->enable_nat = (nat->count > 0);

    char suffix[16];
    generate_random_suffix(suffix, sizeof(suffix));

    if (veth_peer->count == 0) {
      char temp_buf[64];
      snprintf(temp_buf, sizeof(temp_buf), "veth-%s-%s", config->veth_name,
               suffix);
      if (asprintf(&config->veth_peer, "%.15s", temp_buf) < 0) {
        log_fatal("failed to allocate veth peer name");
        exitcode = 1;
        goto cleanup;
      }
    } else {
      config->veth_peer = strdup((char *)veth_peer->sval[0]);
      if (!config->veth_peer) {
        log_fatal("strdup failed");
        exitcode = 1;
        goto cleanup;
      }
    }

    /* 生成临时接口名（用于创建时避免和宿主机接口冲突） */
    char tmp_buf[64];
    snprintf(tmp_buf, sizeof(tmp_buf), "veth-tmp-%s", suffix);
    if (asprintf(&config->veth_tmp, "%.15s", tmp_buf) < 0) {
      log_fatal("failed to allocate veth temp name");
      exitcode = 1;
      goto cleanup;
    }

    log_info("network enabled: %s(%s) <-> %s(%s)", config->veth_name,
             config->container_ip, config->veth_peer,
             config->host_ip ? config->host_ip : "none");
    log_info("dns: %s, nat: %s", config->dns_server,
             config->enable_nat ? "enabled" : "disabled");
  }

  if (geteuid() != 0) {
    log_warn("barco should be run as root for full functionality");
  }

  /* ========== 4. 创建 socket ========== */
  log_debug("creating socket pair for user namespace setup");
  if (socketpair(AF_LOCAL, SOCK_SEQPACKET, 0, sockets) < 0) {
    log_fatal("socketpair failed: %m");
    exitcode = 1;
    goto cleanup;
  }

  if (fcntl(sockets[0], F_SETFD, FD_CLOEXEC) < 0) {
    log_fatal("fcntl FD_CLOEXEC failed: %m");
    exitcode = 1;
    goto cleanup;
  }

  config->fd = sockets[1];

  /* ========== 5. 分配栈空间 ========== */
  log_debug("allocating container stack (%d bytes)", CONTAINER_STACK_SIZE);
  stack = malloc(CONTAINER_STACK_SIZE);
  if (!stack) {
    log_fatal("malloc stack failed: %m");
    exitcode = 1;
    goto cleanup;
  }

  /* ========== 6. 宿主机端网络配置 ========== */
  if (config->veth_name && config->container_ip) {
    log_info("configuring host network: %s <- %s", config->veth_peer,
             config->host_ip ? config->host_ip : "no IP");

    /* 先尝试清理可能残留的同名接口 */
    nl_del_link(config->veth_tmp);
    nl_del_link(config->veth_peer);

    /* 使用临时名称创建 veth，避免和宿主机现有接口冲突 */
    if (nl_create_veth(config->veth_tmp, config->veth_peer) < 0) {
      log_error("failed to create veth pair: %s <-> %s", config->veth_tmp,
                config->veth_peer);
      exitcode = 1;
      goto cleanup;
    }
    veth_created = 1;

    if (config->host_ip) {
      if (nl_add_addr(config->veth_peer, config->host_ip) < 0) {
        log_warn("failed to add IP %s to %s (non-fatal)", config->host_ip,
                 config->veth_peer);
      }
    }

    if (nl_set_up(config->veth_peer) < 0) {
      log_warn("failed to bring up %s (non-fatal)", config->veth_peer);
    }

    if (config->host_ip && config->container_ip) {
      if (enable_ip_forward() < 0) {
        log_warn("failed to enable IP forwarding");
      }
    }

    /* 配置 NAT（如果启用） */
    if (config->enable_nat && config->host_ip) {
      /* 提取子网：从 container_ip 中取网络地址 */
      char subnet[32];
      const char *slash = strchr(config->container_ip, '/');
      if (slash) {
        /* 用 host_ip 的子网（更准确：取容器 IP 所在子网） */
        snprintf(subnet, sizeof(subnet), "%.*s",
                 (int)(slash - config->container_ip), config->container_ip);
        /* 加上 CIDR */
        snprintf(subnet, sizeof(subnet), "%.*s%s",
                 (int)(slash - config->container_ip), config->container_ip,
                 slash);
      } else {
        snprintf(subnet, sizeof(subnet), "%s/24", config->container_ip);
      }

      /* 自动检测宿主机出口接口 */
      if (detect_host_interface(host_if, sizeof(host_if)) < 0) {
        log_warn("failed to detect host interface, NAT may not work");
        snprintf(host_if, sizeof(host_if), "eth0");
      }
      log_info("host interface for NAT: %s", host_if);

      if (setup_nat(subnet, host_if) < 0) {
        log_warn("NAT setup failed (non-fatal)");
      } else {
        nat_applied = 1;
      }
    }

    log_debug("host network configured");
  }

  /* ========== 7. 创建容器进程 ========== */
  log_info("creating container process...");
  container_pid = container_init(config, stack + CONTAINER_STACK_SIZE);
  if (container_pid < 0) {
    log_fatal("container_init failed");
    exitcode = 1;
    goto cleanup;
  }

  /* 短暂等待确保容器进程启动，但子进程会自行等待网络接口 */
  usleep(100000);
  if (kill(container_pid, 0) != 0) {
    log_error("container process exited prematurely (check logs above)");
    exitcode = 1;
    goto cleanup;
  }

  /* ========== 7.5 将容器端 veth 移到容器网络命名空间 ========== */
  if (veth_created && config->veth_tmp) {
    char ns_path[64];
    snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/net", container_pid);
    int ns_fd = open(ns_path, O_RDONLY);
    if (ns_fd < 0) {
      log_error("failed to open container netns: %m");
      container_stop(container_pid);
      exitcode = 1;
      goto cleanup;
    }

    log_debug("moving %s to container netns...", config->veth_tmp);
    if (nl_move_to_ns(config->veth_tmp, ns_fd) < 0) {
      log_error("failed to move %s to container netns", config->veth_tmp);
      close(ns_fd);
      container_stop(container_pid);
      exitcode = 1;
      goto cleanup;
    }
    close(ns_fd);
    log_debug("moved %s to container netns", config->veth_tmp);
  }

  /* ========== 8. 配置 cgroups ========== */
  log_debug("initializing cgroups for pid=%d", container_pid);
  if (cgroups_init(config->hostname, container_pid) < 0) {
    log_warn("cgroups_init failed (non-fatal)");
  }

  /* ========== 9. 配置用户命名空间映射 ========== */
  log_debug("setting up user namespace mappings");
  if (user_namespace_prepare_mappings(container_pid, sockets[0]) < 0) {
    log_error("user namespace setup failed, stopping container");
    container_stop(container_pid);
    exitcode = 1;
    goto cleanup;
  }

  /* ========== 10. 等待容器退出 ========== */
  log_info("container running (pid=%d), waiting...", container_pid);

  int wstatus;
  pid_t w;
  while (1) {
    w = waitpid(container_pid, &wstatus, 0);
    if (w == -1) {
      if (errno == EINTR) {
        continue;
      }
      log_error("waitpid failed: %m");
      exitcode = 1;
      goto cleanup;
    }
    if (w == container_pid) {
      break;
    }
  }

  if (WIFEXITED(wstatus)) {
    container_status = WEXITSTATUS(wstatus);
    log_info("container exited with code %d", container_status);
  } else if (WIFSIGNALED(wstatus)) {
    container_status = 128 + WTERMSIG(wstatus);
    log_warn("container terminated by signal %d", WTERMSIG(wstatus));
  }

  if (exitcode == 0) {
    exitcode = container_status;
  }

  log_debug("container finished with exit code: %d", container_status);

cleanup:
  log_info("freeing resources...");

  /* 清理 NAT 规则 */
  if (nat_applied && config && config->container_ip && host_if[0]) {
    char subnet[32];
    const char *slash = strchr(config->container_ip, '/');
    if (slash) {
      snprintf(subnet, sizeof(subnet), "%s", config->container_ip);
    } else {
      snprintf(subnet, sizeof(subnet), "%s/24", config->container_ip);
    }
    cleanup_nat(subnet, host_if);
  }

  /* 清理网络资源（即使移动失败或容器已退出，此删除会安全进行） */
  if (veth_created && config && config->veth_peer) {
    log_info("removing veth peer: %s", config->veth_peer);
    if (nl_del_link(config->veth_peer) < 0) {
      log_warn("failed to remove veth peer %s", config->veth_peer);
    }
  }

  if (stack) {
    free(stack);
    stack = NULL;
  }

  if (sockets[0] >= 0) {
    close(sockets[0]);
    sockets[0] = -1;
  }
  if (sockets[1] >= 0) {
    close(sockets[1]);
    sockets[1] = -1;
  }

  if (config && config->hostname) {
    log_info("cleaning up cgroups for %s", config->hostname);
    cgroups_free(config->hostname);
  }

exit:
  if (config) {
    free(config->veth_peer);
    free(config->veth_tmp);
    free(config);
    config = NULL;
  }

  arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));

  log_info("so long and thanks for all the fish (exit code: %d)", exitcode);
  return exitcode;
}
