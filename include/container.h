#ifndef CONTAINER_H
#define CONTAINER_H

#include <sys/types.h>

/**
 * 容器相关的常量定义
 */

// 容器进程使用的栈大小：1MB（1024 * 1024 字节）
// clone() 系统调用需要为子进程分配独立的栈空间
// 1MB 是通常足够使用的默认大小，可根据需要调整
enum {
  CONTAINER_STACK_SIZE = (1024 * 1024),
};

/**
 * 容器配置结构体
 *
 * 该结构体包含了创建和运行一个容器所需的所有配置信息。
 * 用于在父进程和子进程（容器进程）之间传递配置参数。
 */
typedef struct {
  uid_t uid;      // 容器内进程的用户 ID（UID）
  int fd;         // 容器使用的 socket 文件描述符
  char *hostname; // 容器的主机名
  char *cmd;      // 容器启动后要执行的命令的完整路径
  char *arg; // 要执行的命令的参数（通常是命令的第 0 个参数）
  char *mnt;       // 容器的根文件系统挂载点（mount point）
  char *veth_name; /* 容器内 veth 接口名，如 "eth0" */
  char *veth_peer; /* 宿主机端 veth 接口名，如 "veth-eth0-xxx" */
  char *veth_tmp; /* 容器端临时接口名（创建时使用，避免和宿主机冲突） */
  char *container_ip; /* 容器 IP/CIDR，如 "10.0.0.2/24" */
  char *host_ip;      /* 宿主机端 IP/CIDR，如 "10.0.0.1/24" */

  /* 新增：NAT 和 DNS */
  char *dns_server; /* DNS 服务器地址 */
  int enable_nat;   /* 是否启用 NAT */
} container_config;

/**
 * 初始化并启动容器
 *
 * 此函数调用 clone() 系统调用创建一个新的容器进程，
 * 并配置各种命名空间（mount、pid、net、uts 等）以实现隔离。
 *
 * @param config 容器配置结构体指针，包含容器的各项配置参数
 * @param stack  子进程使用的栈空间指针（由调用者分配）
 *               大小至少为 CONTAINER_STACK_SIZE 字节
 * @return 成功返回容器进程的 PID（进程 ID），失败返回 1
 */
int container_start(void *arg);

int container_init(container_config *config, char *stack);

/**
 * 等待容器进程退出
 *
 * 此函数会阻塞当前进程，直到指定 PID 的容器进程退出。
 * 用于父进程监控容器进程的生命周期。
 *
 * @param container_pid 容器进程的 PID（由 container_init 返回）
 * @return 容器进程的退出状态码
 *         即容器内主进程调用 exit() 或 main() 函数返回的值
 */
int container_wait(int container_pid);

/**
 * 强制停止容器进程
 *
 * 向容器进程发送 SIGKILL 信号，立即终止该进程。
 * SIGKILL 信号不能被捕获、阻塞或忽略，因此可以保证进程被终止。
 *
 * @param container_pid 要停止的容器进程 PID
 */
void container_stop(int container_pid);

#endif /* CONTAINER_H */