#ifndef SEC_H
#define SEC_H

#include <errno.h>

/**
 * 用于表示 seccomp 规则的违规处理结果
 *
 * 当进程尝试执行被 seccomp 规则禁止的系统调用时，
 * 内核将返回 EPERM 错误码（操作不允许）给调用者，
 * 而不是直接终止进程或忽略该调用。
 *
 * SCMP_ACT_ERRNO: seccomp 动作，表示返回指定的错误码
 * EPERM: Error PERMission，错误码 #1，表示"操作不允许"
 */
#define SEC_SCMP_FAIL SCMP_ACT_ERRNO(EPERM)

/**
 * 为当前调用进程设置能力（capabilities）
 *
 * capabilities 是 Linux 中细粒度的权限控制机制，将传统 root 用户的
 * 全部特权分解为多个独立的能力单元（如 CAP_NET_ADMIN、CAP_SYS_TIME 等）。
 *
 * 该函数通常会移除进程的不必要能力，只保留运行所需的最小权限，
 * 遵循"最小权限原则"以提高安全性。
 *
 * @return 成功返回 0，失败返回 -1 并设置 errno
 */
int sec_set_caps(void);

/**
 * 为当前调用进程设置 seccomp 过滤规则
 *
 * seccomp（secure computing mode）是 Linux 内核提供的系统调用过滤机制，
 * 可以限制进程能够调用的系统调用，从而降低攻击面。
 *
 * 该函数会安装一组预先定义的系统调用白名单或黑名单规则，
 * 当进程尝试调用被禁止的系统调用时，将触发 SEC_SCMP_FAIL 定义的错误处理。
 *
 * 典型应用场景：容器运行时、沙箱环境、高安全性服务进程等。
 *
 * @return 成功返回 0，失败返回 -1 并设置 errno
 */
int sec_set_seccomp(void);

#endif /* SEC_H */